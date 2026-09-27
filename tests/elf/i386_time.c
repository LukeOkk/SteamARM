struct ts32 { long s, ns; }; struct tv32 { long s, us; };
extern int printf(const char *, ...); extern long time(long *); extern int clock_gettime(int, struct ts32 *);
extern int gettimeofday(struct tv32 *, void *); extern long syscall(long, ...); extern void exit(int) __attribute__((noreturn));
struct ts64 { long long s, ns; };
static void run(void) {
    struct ts32 rt, mono; struct tv32 tv; struct ts64 rt64 = {0,0};
    clock_gettime(0, &rt); clock_gettime(1, &mono); gettimeofday(&tv, 0);
    long t13 = syscall(13, 0);                    /* SYS_time */
    long r403 = syscall(403, 0, &rt64);           /* SYS_clock_gettime64 */
    printf("sys_time=%ld libc_time=%ld rt=%ld mono=%ld tod=%ld cg64=%lld(rc %ld)\n", t13, time(0), rt.s, mono.s, tv.s, rt64.s, r403);
}
void _start(void) { run(); exit(0); }
