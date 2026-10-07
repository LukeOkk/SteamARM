/* x11spawn: start a program as a child of SteamARM's X server (its spawn
 * service, patches/xquartz-spawn-service.patch), so that it runs in the
 * coalition of SteamARM-X11.app -- the application macOS Game Mode favours
 * while a game it shows is in full screen.
 *
 *   x11spawn [--wait] [--stdio FILE] SOCKET PROGRAM [ARGS...]
 *
 * Sends the working directory, the arguments (PROGRAM resolved on PATH),
 * this process's environment, and descriptors for 0, 1 and 2 (/dev/null and
 * FILE opened for appending, or this process's own), then prints the pid the
 * server started and exits 0 -- or, with --wait, waits for the program and
 * exits with its status (128 + signal for a death by signal). Exit 125 when
 * the service is not there or refuses: the caller starts the program
 * itself. */
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern char **environ;
#define XQ_SPAWN_MAGIC 0x53504e31u
#define XQ_SPAWN_MAGIC_WAIT 0x53504e32u
struct xq_spawn_hdr { uint32_t magic, argc, envc, len; };

static const char *resolve(const char *prog, char *buf, size_t n)
{
    if (strchr(prog, '/'))
        return realpath(prog, buf) ? buf : NULL;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    char *dup = strdup(path), *save = NULL;
    for (char *d = strtok_r(dup, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(buf, n, "%s/%s", *d ? d : ".", prog);
        if (access(buf, X_OK) == 0) {
            free(dup);
            return buf;
        }
    }
    free(dup);
    return NULL;
}

int main(int argc, char **argv)
{
    int a = 1, wait_for = 0;
    const char *stdio = NULL;
    for (;;) {
        if (a < argc && !strcmp(argv[a], "--wait")) {
            wait_for = 1;
            a++;
        } else if (a + 1 < argc && !strcmp(argv[a], "--stdio")) {
            stdio = argv[a + 1];
            a += 2;
        } else {
            break;
        }
    }
    if (argc - a < 2) {
        fprintf(stderr, "usage: x11spawn [--wait] [--stdio FILE] SOCKET PROGRAM [ARGS...]\n");
        return 64;
    }
    const char *sock = argv[a++];
    char prog[PATH_MAX], cwd[PATH_MAX];
    if (!resolve(argv[a], prog, sizeof prog)) {
        fprintf(stderr, "x11spawn: %s: not found\n", argv[a]);
        return 125;
    }
    if (!getcwd(cwd, sizeof cwd))
        cwd[0] = '\0';

    int fds[3];
    if (stdio) {
        fds[0] = open("/dev/null", O_RDONLY);
        fds[1] = open(stdio, O_WRONLY | O_APPEND | O_CREAT, 0644);
        fds[2] = fds[1];
        if (fds[0] < 0 || fds[1] < 0) {
            fprintf(stderr, "x11spawn: %s: %s\n", stdio, strerror(errno));
            return 125;
        }
    } else {
        fds[0] = 0; fds[1] = 1; fds[2] = 2;
    }

    /* cwd \0 argv[0..] \0 environ[..] \0 */
    size_t len = strlen(cwd) + 1 + strlen(prog) + 1;
    uint32_t nargs = (uint32_t)(argc - a), nenv = 0;
    for (int i = a + 1; i < argc; i++)
        len += strlen(argv[i]) + 1;
    for (char **e = environ; *e; e++, nenv++)
        len += strlen(*e) + 1;
    char *blob = malloc(len), *p = blob;
    if (!blob)
        return 125;
    p = stpcpy(p, cwd) + 1;
    p = stpcpy(p, prog) + 1;
    for (int i = a + 1; i < argc; i++)
        p = stpcpy(p, argv[i]) + 1;
    for (char **e = environ; *e; e++)
        p = stpcpy(p, *e) + 1;

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(sock) >= sizeof sa.sun_path)
        return 125;
    strcpy(sa.sun_path, sock);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "x11spawn: %s: %s\n", sock, strerror(errno));
        return 125;
    }
    struct xq_spawn_hdr h = { wait_for ? XQ_SPAWN_MAGIC_WAIT : XQ_SPAWN_MAGIC, nargs, nenv, (uint32_t)len };
    struct iovec iov = { &h, sizeof h };
    char cbuf[CMSG_SPACE(sizeof fds)];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof fds);
    memcpy(CMSG_DATA(cm), fds, sizeof fds);
    if (sendmsg(s, &m, 0) != (ssize_t)sizeof h) {
        fprintf(stderr, "x11spawn: send: %s\n", strerror(errno));
        return 125;
    }
    for (size_t off = 0; off < len;) {
        ssize_t n = write(s, blob + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fprintf(stderr, "x11spawn: send: %s\n", strerror(errno));
            return 125;
        }
        off += (size_t)n;
    }
    int32_t reply = 0;
    if (read(s, &reply, sizeof reply) != (ssize_t)sizeof reply || reply <= 0) {
        fprintf(stderr, "x11spawn: the server refused (%d)\n", (int)reply);
        return 125;
    }
    if (!wait_for) {
        printf("%d\n", (int)reply);
        return 0;
    }
    int32_t st = 0;
    ssize_t n;
    do {
        n = read(s, &st, sizeof st);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof st)
        return 125;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 125;
}
