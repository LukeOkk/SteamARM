// i386 probe for what the Steam client does right after its UI comes up:
// posix_spawn (glibc: clone(CLONE_VM|CLONE_VFORK)), nested spawns from a
// spawned child, and SysV shared memory through the ipc() multiplexer
// (MIT-SHM). No crt1.o in the rootfs, so _start calls into libc directly.
#include <stddef.h>
typedef int pid_t;
struct shmid_ds;
extern int printf(const char *, ...);
extern int posix_spawn(pid_t *, const char *, const void *, const void *,
                       char *const[], char *const[]);
extern pid_t waitpid(pid_t, int *, int);
extern int shmget(int, size_t, int);
extern void *shmat(int, const void *, int);
extern int shmdt(const void *);
extern int shmctl(int, int, struct shmid_ds *);
extern char **environ;
extern void exit(int) __attribute__((noreturn));

static int spawn_wait(const char *path, char *const argv[]) {
    pid_t pid;
    int st = -1;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (rc != 0) { printf("MAL posix_spawn %s: %d\n", path, rc); return 1; }
    if (waitpid(pid, &st, 0) != pid) { printf("MAL waitpid\n"); return 1; }
    if (st != 0) { printf("MAL %s status 0x%x\n", path, st); return 1; }
    return 0;
}

static int check(void) {
    int bad = 0;
    char *t[] = {"true", NULL};
    for (int i = 0; i < 3; i++)
        bad += spawn_wait("/bin/true", t);
    // A spawned shell that itself spawns (fork in a vfork child's image).
    char *sh[] = {"sh", "-c", "/bin/true && echo nested-ok", NULL};
    bad += spawn_wait("/bin/sh", sh);

    for (int i = 0; i < 4; i++) {
        int id = shmget(0 /* IPC_PRIVATE */, 4096 * (i + 1), 0600 | 01000 /* IPC_CREAT */);
        if (id < 0) { printf("MAL shmget\n"); bad++; break; }
        volatile int *p = shmat(id, NULL, 0);
        if (p == (void *)-1) { printf("MAL shmat\n"); bad++; break; }
        p[0] = 0x5a5a + i;
        if (p[0] != 0x5a5a + i) { printf("MAL shm rw\n"); bad++; }
        if (shmdt((const void *)p) != 0) { printf("MAL shmdt\n"); bad++; }
        shmctl(id, 0 /* IPC_RMID */, NULL);
    }
    printf(bad ? "== i386 spawn: MAL\n" : "== i386 spawn: ok\n");
    return bad;
}
void _start(void) { exit(check()); }
