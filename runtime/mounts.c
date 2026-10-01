// See mounts.h. Evidence for what has to be interpreted:
// benchmarks/stage6-bwrap-plan.txt (the real plan pressure-vessel generates,
// 47 options, no --unshare-*), and pressure-vessel's own strings: it hands the
// plan over as `bwrap --args FD` and "replaces self with bwrap" (execve).
#include "lxrt.h"
#include "mounts.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LERR(e) (-(long)lxrt_errno_to_linux(e))
bool lxrt_trace_on(void);

enum { MAX_MOUNTS = 256 };
struct mount { char dst[PATH_MAX]; char src[PATH_MAX]; bool ro; };
static struct mount g_m[MAX_MOUNTS];
static int g_n;

static const char *guest_to_host(const char *path, char *buf, size_t n);

// --------------------------------------------------------------- the table

static int by_dst_len_desc(const void *a, const void *b)
{
    size_t la = strlen(((const struct mount *)a)->dst);
    size_t lb = strlen(((const struct mount *)b)->dst);
    return la < lb ? 1 : la > lb ? -1 : 0;
}

static void add_mount(const char *dst, const char *src, bool ro)
{
    if (g_n >= MAX_MOUNTS) {
        fprintf(lxrt_trace_stream(), "[lxrt] bwrap: more than %d mounts, dropping %s\n", MAX_MOUNTS, dst);
        return;
    }
    snprintf(g_m[g_n].dst, PATH_MAX, "%s", dst);
    // Canonical on the host side, so that getcwd() -- which the kernel
    // answers canonically (/private/tmp, not /tmp) -- maps back.
    char real[PATH_MAX];
    snprintf(g_m[g_n].src, PATH_MAX, "%s", realpath(src, real) ? real : src);
    g_m[g_n].ro = ro;
    g_n++;
    qsort(g_m, (size_t)g_n, sizeof g_m[0], by_dst_len_desc);
}

bool lxrt_mounts_active(void) { return g_n > 0; }

void lxrt_mounts_bind(const char *dst, const char *src, bool ro)
{
    lxrt_mounts_unbind(dst);
    add_mount(dst, src, ro);
}

bool lxrt_mounts_unbind(const char *dst)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_m[i].dst, dst)) {
            memmove(&g_m[i], &g_m[i + 1], sizeof g_m[0] * (size_t)(g_n - i - 1));
            g_n--;
            return true;
        }
    return false;
}

static char *serialize(void);
const char *lxrt_mounts_exec_env(void)
{
    static char *last;
    if (!g_n) return NULL;
    char *s = serialize();
    if (!s) return NULL;
    free(last);
    last = malloc(strlen(s) + 13);
    if (last) sprintf(last, "LXRT_MOUNTS=%s", s);
    free(s);
    return last;
}

// Is this guest path inside a read-only bind? bwrap makes the mount itself
// read-only; here the dispatcher refuses the write with EROFS instead.
bool lxrt_mounts_readonly(const char *path)
{
    if (!g_n || !path || path[0] != '/')
        return false;
    for (int i = 0; i < g_n; i++) {
        const char *d = g_m[i].dst;
        size_t l = strlen(d);
        if (strncmp(path, d, l) == 0 && (path[l] == '\0' || path[l] == '/' || (l == 1 && d[0] == '/')))
            return g_m[i].ro;   // longest prefix wins: the table is sorted
    }
    return false;
}

// Host path -> guest path, for getcwd(). Binds first (longest source wins is
// not needed: the first match is fine, sources rarely nest), then the root.
const char *lxrt_mounts_untranslate(const char *host, char *buf, size_t n)
{
    if (!host || host[0] != '/')
        return NULL;
    for (int i = 0; i < g_n; i++) {
        size_t l = strlen(g_m[i].src);
        if (l > 1 && strncmp(host, g_m[i].src, l) == 0 && (host[l] == '\0' || host[l] == '/')) {
            snprintf(buf, n, "%s%s", g_m[i].dst, host + l);
            return buf;
        }
    }
    const char *root = getenv("LXRT_ROOT");
    if (root && *root) {
        char real[PATH_MAX];
        const char *r = realpath(root, real) ? real : root;
        size_t l = strlen(r);
        if (strncmp(host, r, l) == 0 && (host[l] == '\0' || host[l] == '/')) {
            snprintf(buf, n, "%s", host[l] ? host + l : "/");
            return buf;
        }
    }
    return NULL;
}

// Symlinks that --symlink created in the sandbox root (/lib -> usr/lib, ...)
// point INTO binds; left to the host kernel they resolve against the empty
// directories standing in for the bind targets. Rewrite them in guest terms
// first, component by component, until the path meets a bind or a real file.
static const char *through_root_symlinks(const char *path, char *out, size_t n)
{
    const char *root = getenv("LXRT_ROOT");
    if (!root || !*root)
        return path;
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", path);
    for (int hops = 0; hops < 40; hops++) {
        bool changed = false;
        // Walk prefixes /a, /a/b, ... that no bind covers.
        for (char *slash = cur + 1; ; slash++) {
            if (*slash != '/' && *slash != '\0')
                continue;
            char save = *slash;
            if (!save)
                break;      // the last component is left alone: readlink,
                            // lstat and unlink act on the link itself
            *slash = '\0';
            bool bound = false;
            for (int i = 0; i < g_n && !bound; i++) {
                size_t l = strlen(g_m[i].dst);
                bound = strncmp(cur, g_m[i].dst, l) == 0 && (cur[l] == '\0' || cur[l] == '/');
            }
            if (bound) { *slash = save; break; }
            char in_root[PATH_MAX], target[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", root, cur);
            ssize_t tl = readlink(in_root, target, sizeof target - 1);
            if (tl > 0) {
                target[tl] = '\0';
                char next[PATH_MAX];
                if (target[0] == '/') {
                    snprintf(next, sizeof next, "%s%s%s", target, save ? "/" : "", save ? slash + 1 : "");
                } else {
                    char *dir = strrchr(cur, '/');
                    *dir = '\0';
                    snprintf(next, sizeof next, "%s/%s%s%s", cur, target, save ? "/" : "", save ? slash + 1 : "");
                }
                snprintf(cur, sizeof cur, "%s", next);
                changed = true;
                break;
            }
            *slash = save;
            if (!save)
                break;
        }
        if (!changed)
            break;
    }
    snprintf(out, n, "%s", cur);
    return out;
}

const char *lxrt_mounts_translate(const char *path, char *buf, size_t n)
{
    if (!g_n || !path || path[0] != '/')
        return NULL;
    char canon[PATH_MAX];
    path = through_root_symlinks(path, canon, sizeof canon);
    for (int i = 0; i < g_n; i++) {
        const char *d = g_m[i].dst;
        size_t l = strlen(d);
        if (strncmp(path, d, l) != 0)
            continue;
        if (path[l] != '\0' && path[l] != '/' && !(l == 1 && d[0] == '/'))
            continue;
        const char *rest = path + (l == 1 && d[0] == '/' ? 0 : l);
        int k = snprintf(buf, n, "%s%s", g_m[i].src, rest);
        if (k <= 0 || (size_t)k >= n)
            return NULL;
        return buf;
    }
    return NULL;
}

// Records: dst \x1e src \x1e ro, separated by \x1f.
void lxrt_mounts_load_env(void)
{
    const char *env = getenv("LXRT_MOUNTS");
    if (!env || !*env)
        return;
    char *copy = strdup(env);
    for (char *rec = strtok(copy, "\x1f"); rec; rec = strtok(NULL, "\x1f")) {
        char *a = strchr(rec, '\x1e');
        if (!a) continue;
        *a++ = 0;
        char *b = strchr(a, '\x1e');
        if (!b) continue;
        *b++ = 0;
        add_mount(rec, a, b[0] == '1');
    }
    free(copy);
}

static char *serialize(void)
{
    size_t need = 1;
    for (int i = 0; i < g_n; i++)
        need += strlen(g_m[i].dst) + strlen(g_m[i].src) + 5;
    char *out = malloc(need);
    if (!out) return NULL;
    size_t at = 0;
    for (int i = 0; i < g_n; i++)
        at += (size_t)snprintf(out + at, need - at, "%s\x1e%s\x1e%c\x1f",
                               g_m[i].dst, g_m[i].src, g_m[i].ro ? '1' : '0');
    return out;
}

// ------------------------------------------------------- the interpreter

// Read `--args FD`: NUL-separated arguments, appended to the vector.
static int read_args_fd(int fd, char ***vec, int *cnt, int *cap)
{
    size_t sz = 0, capb = 65536;
    char *buf = malloc(capb);
    if (!buf) return -ENOMEM;
    for (;;) {
        if (sz == capb) { capb *= 2; char *nb = realloc(buf, capb); if (!nb) { free(buf); return -ENOMEM; } buf = nb; }
        ssize_t r = read(fd, buf + sz, capb - sz);
        if (r < 0) { if (errno == EINTR) continue; int e = errno; free(buf); return -e; }
        if (r == 0) break;
        sz += (size_t)r;
    }
    size_t at = 0;
    while (at < sz) {
        if (*cnt + 1 >= *cap) { *cap *= 2; *vec = realloc(*vec, (size_t)*cap * sizeof(char *)); }
        (*vec)[(*cnt)++] = strdup(buf + at);
        at += strlen(buf + at) + 1;
    }
    free(buf);
    return 0;
}

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, mode); *p = '/'; }
    return mkdir(tmp, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static int mkparents(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *s = strrchr(tmp, '/');
    if (!s || s == tmp) return 0;
    *s = 0;
    return mkdirs(tmp, 0755);
}

static const char *guest_to_host(const char *path, char *buf, size_t n);

long lxrt_bwrap_exec(char *const argv0[], char *const envp[],
                     long (*exec_guest)(const char *, char *const[], char *const[]))
{
    // Copy the vector so --args can extend it.
    int cap = 64, cnt = 0;
    char **v = malloc((size_t)cap * sizeof(char *));
    for (int i = 0; argv0[i]; i++) {
        if (cnt + 1 >= cap) { cap *= 2; v = realloc(v, (size_t)cap * sizeof(char *)); }
        v[cnt++] = argv0[i];
    }
    v[cnt] = NULL;

    char sandbox[PATH_MAX];
    snprintf(sandbox, sizeof sandbox, "%s/lxrt-sandbox-XXXXXX", lxrt_host_tmpdir());
    if (!mkdtemp(sandbox))
        return LERR(errno);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] bwrap: sandbox root %s\n", sandbox);

    // Environment edits (--setenv/--unsetenv) accumulate here.
    int ecap = 64, ecnt = 0;
    char **env = malloc((size_t)ecap * sizeof(char *));
    for (int i = 0; envp && envp[i]; i++) {
        if (ecnt + 1 >= ecap) { ecap *= 2; env = realloc(env, (size_t)ecap * sizeof(char *)); }
        env[ecnt++] = envp[i];
    }
    env[ecnt] = NULL;
    char chdir_to[PATH_MAX] = "";
    mode_t perms = 0;

    // The emulator's own files stay visible, as they do on Linux, where FEX
    // reaches its x86 rootfs and config through what it opened before bwrap
    // ran. Here every process re-reads them by path, so without these binds
    // an x86 program in the sandbox had no loader and no libc. Resolved in
    // the CALLER's view, before the plan's own binds enter the table.
    char emu_dst[3][PATH_MAX], emu_src[3][PATH_MAX];
    int n_emu = 0;
    {
        char cfg[PATH_MAX] = "";
        const char *home = getenv("HOME");
        if (home && home[0] == '/')
            snprintf(cfg, sizeof cfg, "%s/.fex-emu", home);
        // /usr/lib/lxrt-emu: FEX and the aarch64 loader and libraries it is
        // linked against (PT_INTERP and DT_RPATH point there), the part of
        // the emulator that is ELF files rather than runtime.
        const char *emu[] = { getenv("FEX_ROOTFS"), cfg[0] ? cfg : NULL, "/usr/lib/lxrt-emu" };
        for (unsigned k = 0; k < sizeof emu / sizeof emu[0]; k++) {
            const char *p = emu[k];
            if (!p || p[0] != '/' || !p[1])
                continue;                   // "/" is no overlay: nothing to keep
            char host[PATH_MAX];
            const char *h = guest_to_host(p, host, sizeof host);
            struct stat st;
            if (!h || stat(h, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            snprintf(emu_dst[n_emu], PATH_MAX, "%s", p);
            snprintf(emu_src[n_emu], PATH_MAX, "%s", h);
            n_emu++;
        }
    }

    int i = 1;
    for (; i < cnt; i++) {
        const char *o = v[i];
        if (strcmp(o, "--") == 0) { i++; break; }
        if (o[0] != '-' || o[1] != '-') break;          // the command starts
        if (!strcmp(o, "--version")) {
            // pressure-vessel probes its bwrap before using it. Say which
            // bwrap this stands in for: the plan semantics are those of 0.10.
            static const char msg[] = "bubblewrap 0.10.0 (lxrt interpreter)\n";
            ssize_t w = write(1, msg, sizeof msg - 1);
            (void)w;
            _exit(0);
        }
        #define NEED(k) do { if (i + (k) >= cnt) { fprintf(lxrt_trace_stream(), "[lxrt] bwrap: %s needs %d argument(s)\n", o, (k)); return LERR(EINVAL); } } while (0)
        if (!strcmp(o, "--args")) {
            NEED(1);
            int fd = atoi(v[i + 1]);
            // Splice the fd's arguments in place of these two.
            char **extra = malloc(16 * sizeof(char *)); int ec = 0, ecap2 = 16;
            int r = read_args_fd(fd, &extra, &ec, &ecap2);
            if (r < 0) return LERR(-r);
            int tail = cnt - (i + 2);
            char **nv = malloc((size_t)(cnt - 2 + ec + 1) * sizeof(char *));
            memcpy(nv, v, (size_t)i * sizeof(char *));
            memcpy(nv + i, extra, (size_t)ec * sizeof(char *));
            memcpy(nv + i + ec, v + i + 2, (size_t)tail * sizeof(char *));
            cnt = i + ec + tail; nv[cnt] = NULL; v = nv;
            close(fd);
            i--;                                          // re-examine at i
            continue;
        }
        if (!strcmp(o, "--ro-bind") || !strcmp(o, "--bind") || !strcmp(o, "--dev-bind") ||
            !strcmp(o, "--ro-bind-try") || !strcmp(o, "--bind-try") || !strcmp(o, "--dev-bind-try")) {
            NEED(2);
            const char *src = v[i + 1], *dst = v[i + 2];
            // /proc, /sys and /dev onto themselves: those trees are the
            // runtime's own (procfs.c, proc_ext.c, the /dev passthrough) in
            // every process, sandboxed
            // or not, so the bind is already in effect. check-requirements
            // binds both before running `true`.
            if ((!strcmp(src, "/proc") && !strcmp(dst, "/proc")) ||
                (!strcmp(src, "/sys") && !strcmp(dst, "/sys")) ||
                (!strcmp(src, "/dev") && !strcmp(dst, "/dev"))) {
                i += 2;
                continue;
            }
            char host[PATH_MAX];
            const char *h = guest_to_host(src, host, sizeof host);
            struct stat st;
            if (!h || stat(h, &st) != 0) {
                if (strstr(o, "-try")) { i += 2; continue; }
                fprintf(lxrt_trace_stream(), "[lxrt] bwrap: %s %s: source missing (%s)\n", o, src, h ? h : src);
                return LERR(ENOENT);
            }
            // The destination has to exist in the sandbox root for a guest
            // that stats the parent directory; the bind itself is a table entry.
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, dst);
            if (S_ISDIR(st.st_mode)) mkdirs(in_root, 0755);
            else { mkparents(in_root); int fd = open(in_root, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); }
            add_mount(dst, h, strncmp(o, "--ro", 4) == 0);
            i += 2; continue;
        }
        if (!strcmp(o, "--symlink")) {
            NEED(2);
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, v[i + 2]);
            mkparents(in_root);
            unlink(in_root);
            if (symlink(v[i + 1], in_root) != 0 && errno != EEXIST)
                return LERR(errno);
            i += 2; continue;
        }
        if (!strcmp(o, "--tmpfs") || !strcmp(o, "--dir")) {
            NEED(1);
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, v[i + 1]);
            if (mkdirs(in_root, perms ? perms : 0755) != 0) return LERR(errno);
            perms = 0;
            i += 1; continue;
        }
        if (!strcmp(o, "--proc") || !strcmp(o, "--dev")) {
            NEED(1);
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, v[i + 1]);
            mkdirs(in_root, 0755);
            if (!strcmp(o, "--dev")) add_mount(v[i + 1], "/dev", false);
            // /proc is synthesised by procfs.c wherever it is asked for.
            i += 1; continue;
        }
        if (!strcmp(o, "--ro-bind-data") || !strcmp(o, "--bind-data") || !strcmp(o, "--file")) {
            NEED(2);
            int fd = atoi(v[i + 1]);
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, v[i + 2]);
            mkparents(in_root);
            int out = open(in_root, O_CREAT | O_WRONLY | O_TRUNC, perms ? perms : 0644);
            perms = 0;
            if (out < 0) return LERR(errno);
            char buf[65536]; ssize_t r;
            while ((r = read(fd, buf, sizeof buf)) > 0) write(out, buf, (size_t)r);
            close(out); close(fd);
            i += 2; continue;
        }
        if (!strcmp(o, "--setenv")) {
            NEED(2);
            size_t l = strlen(v[i + 1]);
            char *kv = malloc(l + strlen(v[i + 2]) + 2);
            sprintf(kv, "%s=%s", v[i + 1], v[i + 2]);
            bool replaced = false;
            for (int k = 0; k < ecnt; k++)
                if (strncmp(env[k], v[i + 1], l) == 0 && env[k][l] == '=') { env[k] = kv; replaced = true; break; }
            if (!replaced) { if (ecnt + 1 >= ecap) { ecap *= 2; env = realloc(env, (size_t)ecap * sizeof(char *)); } env[ecnt++] = kv; env[ecnt] = NULL; }
            i += 2; continue;
        }
        if (!strcmp(o, "--unsetenv")) {
            NEED(1);
            size_t l = strlen(v[i + 1]);
            for (int k = 0; k < ecnt; k++)
                if (strncmp(env[k], v[i + 1], l) == 0 && env[k][l] == '=') { env[k] = env[--ecnt]; env[ecnt] = NULL; break; }
            i += 1; continue;
        }
        if (!strcmp(o, "--chdir")) { NEED(1); snprintf(chdir_to, sizeof chdir_to, "%s", v[i + 1]); i += 1; continue; }
        if (!strcmp(o, "--perms")) { NEED(1); perms = (mode_t)strtol(v[i + 1], NULL, 8); i += 1; continue; }
        if (!strcmp(o, "--chmod")) {
            // bwrap: --chmod OCTAL PATH (two arguments), on a path the plan
            // already made. Only what the plan created in the sandbox is
            // changed: a bind's placeholder is, its host source is not.
            NEED(2);
            char in_root[PATH_MAX];
            snprintf(in_root, sizeof in_root, "%s%s", sandbox, v[i + 2]);
            if (chmod(in_root, (mode_t)strtol(v[i + 1], NULL, 8)) != 0)
                fprintf(lxrt_trace_stream(), "[lxrt] bwrap: --chmod %s %s: %s\n", v[i + 1], v[i + 2], strerror(errno));
            i += 2; continue;
        }
        if (!strcmp(o, "--sync-fd") || !strcmp(o, "--info-fd") || !strcmp(o, "--json-status-fd") ||
            !strcmp(o, "--block-fd") || !strcmp(o, "--userns-block-fd") || !strcmp(o, "--lock-file") ||
            !strcmp(o, "--remount-ro") || !strcmp(o, "--exec-label") ||
            !strcmp(o, "--file-label") || !strcmp(o, "--hostname") || !strcmp(o, "--argv0")) {
            // fds that bwrap would hold or signal on: nothing to hold here.
            NEED(1); i += 1; continue;
        }
        if (!strcmp(o, "--new-session") || !strcmp(o, "--die-with-parent") || !strcmp(o, "--level-prefix") ||
            !strcmp(o, "--not-a-security-boundary") || !strcmp(o, "--as-pid-1") || !strcmp(o, "--clearenv") ||
            !strcmp(o, "--disable-userns") || !strcmp(o, "--assert-userns-disabled") || !strcmp(o, "--unshare-all") ||
            !strncmp(o, "--unshare-", 10) || !strcmp(o, "--cap-add") || !strcmp(o, "--cap-drop") ||
            !strcmp(o, "--uid") || !strcmp(o, "--gid") || !strcmp(o, "--seccomp") || !strcmp(o, "--add-seccomp-fd")) {
            bool refuse = !strncmp(o, "--unshare-", 10) || !strcmp(o, "--cap-add") || !strcmp(o, "--cap-drop") ||
                          !strcmp(o, "--uid") || !strcmp(o, "--gid") || !strcmp(o, "--seccomp") || !strcmp(o, "--add-seccomp-fd");
            if (refuse) {
                // The evidence says pressure-vessel never asks for these. If a
                // future one does, say so rather than pretend: a sandbox that
                // silently is not one is worse than no sandbox.
                fprintf(lxrt_trace_stream(), "[lxrt] bwrap: %s is not implementable on Darwin (no namespaces/capabilities)\n", o);
                return LERR(ENOSYS);
            }
            if (!strcmp(o, "--cap-add") || !strcmp(o, "--cap-drop") || !strcmp(o, "--uid") || !strcmp(o, "--gid") || !strcmp(o, "--seccomp") || !strcmp(o, "--add-seccomp-fd")) i += 1;
            continue;
        }
        fprintf(lxrt_trace_stream(), "[lxrt] bwrap: unknown option %s\n", o);
        return LERR(EINVAL);
        #undef NEED
    }
    if (i >= cnt) { fprintf(lxrt_trace_stream(), "[lxrt] bwrap: no command\n"); return LERR(EINVAL); }

    // (emulator binds: resolved before the plan, applied after it -- below)
    for (int k = 0; k < n_emu; k++) {
        char in_root[PATH_MAX];
        snprintf(in_root, sizeof in_root, "%s%s", sandbox, emu_dst[k]);
        mkdirs(in_root, 0755);
        add_mount(emu_dst[k], emu_src[k], true);
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] bwrap: emulator bind %s -> %s\n", emu_dst[k], emu_src[k]);
    }

    // The child's view: the sandbox directory is its root, the binds its table.
    char *ser = serialize();
    char rootvar[PATH_MAX + 16], mountsvar[16];
    snprintf(rootvar, sizeof rootvar, "LXRT_ROOT=%s", sandbox);
    (void)mountsvar;
    size_t ml = strlen(ser ? ser : "") + 16;
    char *mounts_kv = malloc(ml);
    snprintf(mounts_kv, ml, "LXRT_MOUNTS=%s", ser ? ser : "");
    bool set_root = false, set_mounts = false;
    for (int k = 0; k < ecnt; k++) {
        if (!strncmp(env[k], "LXRT_ROOT=", 10)) { env[k] = rootvar; set_root = true; }
        if (!strncmp(env[k], "LXRT_MOUNTS=", 12)) { env[k] = mounts_kv; set_mounts = true; }
    }
    if (ecnt + 2 >= ecap) { ecap += 4; env = realloc(env, (size_t)ecap * sizeof(char *)); }
    if (!set_root) env[ecnt++] = rootvar;
    if (!set_mounts) env[ecnt++] = mounts_kv;
    env[ecnt] = NULL;
    if (chdir_to[0]) {
        char host[PATH_MAX];
        // The cwd is a host notion; take it through the child's own view.
        const char *h = lxrt_mounts_translate(chdir_to, host, sizeof host);
        if (!h) { snprintf(host, sizeof host, "%s%s", sandbox, chdir_to); h = host; }
        if (chdir(h) != 0)
            fprintf(lxrt_trace_stream(), "[lxrt] bwrap: chdir %s: %s\n", chdir_to, strerror(errno));
    }
    // bwrap runs its command with execvp: a bare name is looked up in PATH
    // (steam-runtime-check-requirements runs plain `true`). Resolved in the
    // caller's view; the binds that matter (/usr) carry it into the child's.
    const char *cmd = v[i];
    static char found[PATH_MAX];
    if (!strchr(cmd, '/')) {
        const char *pathv = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        for (int k = 0; k < ecnt; k++)
            if (!strncmp(env[k], "PATH=", 5)) { pathv = env[k] + 5; break; }
        const char *d = pathv;
        while (*d) {
            const char *e = strchr(d, ':');
            size_t dl = e ? (size_t)(e - d) : strlen(d);
            char cand[PATH_MAX], host[PATH_MAX];
            if (dl && dl + strlen(cmd) + 2 < sizeof cand) {
                snprintf(cand, sizeof cand, "%.*s/%s", (int)dl, d, cmd);
                struct stat st;
                char ov[PATH_MAX];
                const char *rootfs = getenv("FEX_ROOTFS");
                bool in_rootfs = false;
                if (rootfs && rootfs[0] == '/') {
                    char rc[PATH_MAX];
                    snprintf(rc, sizeof rc, "%s%s", rootfs, cand);
                    in_rootfs = stat(guest_to_host(rc, ov, sizeof ov), &st) == 0 && S_ISREG(st.st_mode);
                }
                if (in_rootfs ||
                    (stat(guest_to_host(cand, host, sizeof host), &st) == 0 && S_ISREG(st.st_mode))) {
                    snprintf(found, sizeof found, "%s", cand);
                    cmd = found;
                    break;
                }
            }
            if (!e) break;
            d = e + 1;
        }
    }
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] bwrap: %d mounts, exec %s\n", g_n, cmd);
    return exec_guest(cmd, v + i, env);
}

// A bind SOURCE is a path in the parent's view: through its own table first,
// then its root, then the host.
static const char *guest_to_host(const char *path, char *buf, size_t n)
{
    const char *m = lxrt_mounts_translate(path, buf, n);
    if (m) return m;
    const char *root = getenv("LXRT_ROOT");
    if (root && *root && path[0] == '/') {
        char cand[PATH_MAX];
        snprintf(cand, sizeof cand, "%s%s", root, path);
        struct stat st;
        if (stat(cand, &st) == 0) { snprintf(buf, n, "%s", cand); return buf; }
    }
    snprintf(buf, n, "%s", path);
    return buf;
}
