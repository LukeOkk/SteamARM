// Linux abstract socket names under lxrun (materialised in a directory of
// the runtime's): a name with '/' and '%', a 100-byte name, and one bound
// with sizeof(struct sockaddr_un) (NUL-padded) must bind, take a connection
// and come back from getsockname as the same abstract address -- Android's
// zygote checks a child zygote's socket by that name.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;

static void one(const char *label, const char *name, size_t len, int padded)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, name, len);
    socklen_t alen = padded ? sizeof a : (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);
    int s = socket(AF_UNIX, SOCK_STREAM, 0), c = socket(AF_UNIX, SOCK_STREAM, 0);
    int ok = bind(s, (struct sockaddr *)&a, alen) == 0 && listen(s, 1) == 0 &&
             connect(c, (struct sockaddr *)&a, alen) == 0;
    struct sockaddr_un g;
    socklen_t glen = sizeof g;
    memset(&g, 0, sizeof g);
    ok = ok && getsockname(s, (struct sockaddr *)&g, &glen) == 0 && g.sun_path[0] == '\0' &&
         glen >= offsetof(struct sockaddr_un, sun_path) + 1 + len &&
         memcmp(g.sun_path + 1, name, len) == 0;
    printf("  %s  %s (%zu bytes)\n", ok ? "OK " : "MAL", label, len);
    if (!ok) fails++;
    close(c);
    close(s);
}

int main(void)
{
    char pid[32];
    snprintf(pid, sizeof pid, "%d", getpid());
    char n1[128], n2[128], n3[128];
    snprintf(n1, sizeof n1, "com.example.Zygote/%s/50%%", pid);
    memset(n2, 'x', 100);
    memcpy(n2, pid, strlen(pid));
    snprintf(n3, sizeof n3, "padded-%s", pid);
    one("'/' and '%' in the name", n1, strlen(n1), 0);
    one("a 100-byte name", n2, 100, 0);
    one("bound with sizeof(struct sockaddr_un)", n3, strlen(n3), 1);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
