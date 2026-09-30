// SHM_MREMAP: growing a MAP_SHARED file mapping with mremap keeps it a shared
// mapping OF THE FILE (runtime/mremap.c, remap_shared_file). This is what
// every libwayland-server compositor does to a client's wl_shm pool on
// wl_shm_pool.resize (mremap(MREMAP_MAYMOVE) of the memfd the client sent);
// before, lxrun refused ("file-backed range ... not supported yet") and
// Weston killed its own desktop shell: "wl_shm_pool#3: error 2: failed
// mremap" (benchmarks/stage27-android-display.txt).
//
// The pattern checked is the compositor/client one: a "client" process owns
// a memfd and writes into its own mapping; the "server" (this process) maps
// the same memfd, grows its mapping after the client grew the file, and
// reads the client's new bytes through the grown mapping -- and writes that
// the client then sees.
//
// Checks (each "ok"/"MAL"; last line "== shm mremap: ok" or "... N mal"):
//   maymove    mremap(MREMAP_MAYMOVE) 64 KiB -> 1 MiB: the old bytes are there
//   file       a byte written through the grown mapping beyond the old end
//              is in the file (pread), and pwrite is seen through it
//   peer       another process's writes to the file past the old end are seen
//              through the grown mapping, and ours through its mapping
//   inplace    growth without MAYMOVE into free space right after the
//              mapping keeps the address and the file backing
//   shrink     shrinking keeps the address; growing it again still works
//   fixed      MREMAP_MAYMOVE|MREMAP_FIXED to a chosen address, file-backed
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int mals;
static void check(const char *name, int ok, const char *what)
{
    printf("  %s %-8s %s\n", ok ? "ok " : "MAL", name, what);
    if (!ok) mals++;
}

int main(void)
{
    const size_t K64 = 64 << 10, M1 = 1 << 20;
    int fd = memfd_create("shm_mremap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, K64) != 0) { perror("memfd"); return 2; }
    unsigned char *a = mmap(NULL, K64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED) { perror("mmap"); return 2; }
    for (size_t i = 0; i < K64; i++) a[i] = (unsigned char)(i * 7 + 3);

    // maymove: the file grows first (the client's ftruncate), then the map.
    if (ftruncate(fd, M1) != 0) { perror("ftruncate"); return 2; }
    unsigned char *b = mremap(a, K64, M1, MREMAP_MAYMOVE);
    int ok = b != MAP_FAILED;
    for (size_t i = 0; ok && i < K64; i++) ok = b[i] == (unsigned char)(i * 7 + 3);
    check("maymove", ok, b == MAP_FAILED ? strerror(errno) : "64 KiB -> 1 MiB, old bytes kept");
    if (b == MAP_FAILED) { printf("== shm mremap: %d mal\n", mals + 5); return 1; }

    // file: through the grown part, both ways.
    b[900 << 10] = 0x5a;
    unsigned char c = 0;
    ok = pread(fd, &c, 1, 900 << 10) == 1 && c == 0x5a;
    unsigned char w = 0xa5;
    ok = ok && pwrite(fd, &w, 1, (900 << 10) + 1) == 1 && b[(900 << 10) + 1] == 0xa5;
    check("file", ok, "a store past the old end is in the file, and pwrite shows in the mapping");

    // peer: a second process with its own mapping of the same memfd.
    int to_child[2], to_parent[2];
    if (pipe(to_child) || pipe(to_parent)) { perror("pipe"); return 2; }
    pid_t pid = fork();
    if (pid == 0) {
        unsigned char *m = mmap(NULL, M1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) _exit(3);
        m[700 << 10] = 0x42;                    // past the server's old end
        char x = 1;
        if (write(to_parent[1], &x, 1) != 1) _exit(4);
        if (read(to_child[0], &x, 1) != 1) _exit(5);
        _exit(m[600 << 10] == 0x24 ? 0 : 6);    // the server's store
    }
    char x;
    ok = read(to_parent[0], &x, 1) == 1 && b[700 << 10] == 0x42;
    b[600 << 10] = 0x24;
    x = 1;
    ok = write(to_child[1], &x, 1) == 1 && ok;
    int st = 0;
    waitpid(pid, &st, 0);
    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    check("peer", ok, "another process's stores past the old end seen through the grown mapping, and ours in its");

    // inplace: reserve 256 KiB, map the file on the first 64 KiB of it, free
    // the rest, grow without MAYMOVE.
    unsigned char *r = mmap(NULL, 256 << 10, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *d = mmap(r, K64, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    munmap(r + K64, (256 << 10) - K64);
    unsigned char *e = mremap(d, K64, 192 << 10, 0);
    ok = d != MAP_FAILED && e == d && e[0] == (unsigned char)3 && e[(128 << 10) + 5] == b[(128 << 10) + 5];
    b[(160 << 10)] = 0x77;
    ok = ok && e[160 << 10] == 0x77;
    check("inplace", ok, e == MAP_FAILED ? strerror(errno) : "64 -> 192 KiB at the same address, file-backed past the old end");
    if (e != MAP_FAILED) munmap(e, 192 << 10);

    // shrink, then grow again.
    unsigned char *s = mremap(b, M1, 128 << 10, 0);
    unsigned char *g = s == b ? mremap(s, 128 << 10, M1, MREMAP_MAYMOVE) : MAP_FAILED;
    ok = s == b && g != MAP_FAILED && g[900 << 10] == 0x5a && g[700 << 10] == 0x42;
    check("shrink", ok, "1 MiB -> 128 KiB in place, then back to 1 MiB with the file's bytes");

    // fixed: to a chosen free address.
    unsigned char *t = mmap(NULL, 2 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(t, 2 << 20);
    unsigned char *f = g != MAP_FAILED ? mremap(g, M1, M1, MREMAP_MAYMOVE | MREMAP_FIXED, t) : MAP_FAILED;
    ok = f == t && f[900 << 10] == 0x5a;
    w = 0x3c;
    ok = ok && pwrite(fd, &w, 1, 1000 << 10) == 1 && f[1000 << 10] == 0x3c;
    check("fixed", ok, f == MAP_FAILED ? strerror(errno) : "moved to the address asked for, still the file");

    if (mals) printf("== shm mremap: %d mal\n", mals);
    else printf("== shm mremap: ok\n");
    return mals ? 1 : 0;
}
