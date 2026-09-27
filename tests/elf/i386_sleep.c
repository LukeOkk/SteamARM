// i386 probe: sleeps must last as long as asked. The Steam client waits for
// steamwebhelper with 2400 x ThreadSleep(50 ms) (120 s) and gave up after
// ~16 s: each 50 ms sleep came back in a few ms.
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));
extern int usleep(unsigned);
struct ts32 { long s, ns; };
struct tv32 { long s, us; };
extern int nanosleep(const struct ts32 *, struct ts32 *);
extern int clock_nanosleep(int, int, const struct ts32 *, struct ts32 *);
extern int clock_gettime(int, struct ts32 *);
extern int select(int, void *, void *, void *, struct tv32 *);
extern int poll(void *, unsigned long, int);

static long long now_ms(void) { struct ts32 t; clock_gettime(1, &t); return (long long)t.s * 1000 + t.ns / 1000000; }

static int bad;
static void report(const char *what, long long ms)
{
    int ok = ms >= 48 && ms < 200;
    bad += !ok;
    printf("%-26s %lld ms %s\n", what, ms, ok ? "" : "  <-- WRONG");
}

static int check(void)
{
    long long t;
    t = now_ms(); usleep(50000); report("usleep(50000)", now_ms() - t);
    struct ts32 r = { 0, 50000000 };
    t = now_ms(); nanosleep(&r, 0); report("nanosleep(50 ms)", now_ms() - t);
    t = now_ms(); clock_nanosleep(1, 0, &r, 0); report("clock_nanosleep(50 ms)", now_ms() - t);
    struct tv32 tv = { 0, 50000 };
    t = now_ms(); select(0, 0, 0, 0, &tv); report("select(50 ms)", now_ms() - t);
    t = now_ms(); poll(0, 0, 50); report("poll(50 ms)", now_ms() - t);
    t = now_ms(); for (int i = 0; i < 20; i++) usleep(5000); report("20 x usleep(5000)", now_ms() - t + 0);
    printf(bad ? "== sleep: MAL\n" : "== sleep: ok\n");
    return bad;
}
void _start(void) { exit(check()); }
