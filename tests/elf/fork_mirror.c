// fork() against the page-protection lock. The runtime's fork prepare
// handlers take its locks in the reverse of their registration order; the
// shared-page mirror's lock (shmirror.c) was taken before the page lock
// (wxsplit.c), while a thread placing a sub-page mapping holds the page lock
// and then takes the mirror's. Steam's web helper (native arm64) deadlocked
// that way: one thread in fork's prepare handlers, one in
// subpage_mmap_locked -> lxrt_shmirror_take_view, two more behind them in
// epoll_pwait, and no sign-in window (MEASURED).
//
// Thread A: a read-only MAP_SHARED 4 KiB file view at the start of a host
// page (the mirror notes it), then an anonymous 4 KiB mapping MAP_FIXED next
// to it in the same host page (the page goes composite: page lock, then the
// mirror's lock), then unmap; over and over. Thread B: fork(), the child
// exits at once, over and over. Both must finish.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop;
static atomic_long maps, forks;
static int fd;

static void *mapper(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop)) {
        // A 64 KiB hole, 16 KiB-aligned, to place the views in.
        char *hole = mmap(NULL, 64 * 1024, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (hole == MAP_FAILED) continue;
        char *base = (char *)(((unsigned long)hole + 16383) & ~16383ul);
        munmap(hole, 64 * 1024);
        char *v = mmap(base, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
        if (v != MAP_FAILED) {
            char *a = mmap(base + 4096, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (a != MAP_FAILED) a[0] = 1;
            munmap(base, 16384);
            atomic_fetch_add(&maps, 1);
        }
    }
    return NULL;
}

static void *forker(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop)) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        if (p > 0) { waitpid(p, NULL, 0); atomic_fetch_add(&forks, 1); }
    }
    return NULL;
}

int main(void)
{
    char path[] = "/tmp/lxrt-fork-mirror-XXXXXX";
    fd = mkstemp(path);
    unlink(path);
    char page[16384] = { 7 };
    write(fd, page, sizeof page);
    pthread_t a, b;
    pthread_create(&a, NULL, mapper, NULL);
    pthread_create(&b, NULL, forker, NULL);
    sleep(4);
    atomic_store(&stop, 1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("fork_mirror: %ld sub-page placements, %ld forks, no deadlock\n",
           (long)atomic_load(&maps), (long)atomic_load(&forks));
    return atomic_load(&maps) > 0 && atomic_load(&forks) > 0 ? 0 : 1;
}
