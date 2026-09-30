// A bionic program linked against the Android image's own libc.so (there is
// no NDK here): waits with __system_property_wait for a property to take a
// value another process sets through the property service
// (runtime/propsvc.c), as android::base::WaitForProperty does.
//
//   props_wait wait <name> <value> <timeout-s>   "woke: <name>=<value> after N ms"
//   props_wait get <name>                        the value, via
//                                                __system_property_read_callback
//
// Built by tests/android/run.sh (and for x86_64 with
// --target=x86_64-linux-android30 against the x86_64 image's libc.so):
//   clang --target=aarch64-linux-android30 -nostdlib -fPIE -pie
//         -fuse-ld=lld -Wl,--dynamic-linker=/system/bin/linker64
//         props_wait.c <root>/system/lib64/libc.so
// The declarations below are bionic's public ones (libc/include/sys/
// system_properties.h, stdio.h, stdlib.h, time.h); _start is crtbegin.c's.
#include <stddef.h>
#include <stdint.h>

typedef struct prop_info prop_info;
struct timespec { long tv_sec; long tv_nsec; };
const prop_info *__system_property_find(const char *name);
void __system_property_read_callback(const prop_info *pi,
                                     void (*callback)(void *cookie, const char *name,
                                                      const char *value, uint32_t serial),
                                     void *cookie);
int __system_property_wait(const prop_info *pi, uint32_t old_serial, uint32_t *new_serial_ptr,
                           const struct timespec *relative_timeout);
uint32_t __system_property_serial(const prop_info *pi);
uint32_t __system_property_area_serial(void);
int printf(const char *fmt, ...);
int strcmp(const char *a, const char *b);
char *strncpy(char *d, const char *s, size_t n);
int atoi(const char *s);
int clock_gettime(int clk, struct timespec *ts);
int fflush(void *stream);
void exit(int status);

struct value { char v[128]; uint32_t serial; };
static void got(void *cookie, const char *name, const char *value, uint32_t serial)
{
    struct value *out = cookie;
    strncpy(out->v, value, sizeof out->v - 1);
    out->v[sizeof out->v - 1] = 0;
    out->serial = serial;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv, char **envp)
{
    if (argc >= 3 && !strcmp(argv[1], "get")) {
        const prop_info *pi = __system_property_find(argv[2]);
        if (!pi) { printf("(unset)\n"); return 1; }
        struct value v = { "", 0 };
        __system_property_read_callback(pi, got, &v);
        printf("%s\n", v.v);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "follow")) {
        // Every change of <name> until it reads <last>: "<value> <CLOCK_REALTIME ns>"
        // at each wakeup (benchmarks/stage26: wake latency against the
        // setter's clock).
        const prop_info *pi = __system_property_find(argv[2]);
        if (!pi) { printf("(unset)\n"); return 1; }
        static char seen[4096][16];
        static long long when[4096];
        int n = 0;
        struct value v = { "", 0 };
        __system_property_read_callback(pi, got, &v);
        printf("following %s from %s\n", argv[2], v.v);
        fflush(NULL);
        while (strcmp(v.v, argv[3]) != 0 && n < 4096) {
            uint32_t ns;
            struct timespec rel = { 10, 0 };
            if (!__system_property_wait(pi, v.serial, &ns, &rel)) { printf("timeout\n"); break; }
            struct timespec ts;
            clock_gettime(0 /* CLOCK_REALTIME */, &ts);
            __system_property_read_callback(pi, got, &v);
            strncpy(seen[n], v.v, 15);
            when[n++] = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        }
        for (int i = 0; i < n; i++) printf("%s %lld\n", seen[i], when[i]);
        return 0;
    }
    if (argc < 5 || strcmp(argv[1], "wait") != 0) {
        printf("usage: props_wait wait <name> <value> <timeout-s> | get <name> | follow <name> <last>\n");
        return 2;
    }
    const char *name = argv[2], *want = argv[3];
    long deadline = now_ms() + atoi(argv[4]) * 1000L, t0 = now_ms();
    int wakeups = 0;
    // WaitForProperty: until the property exists, wait on the global serial
    // (a new property bumps it); then on the property's own serial.
    const prop_info *pi = NULL;
    uint32_t serial = __system_property_area_serial();
    printf("waiting for %s=%s\n", name, want);
    fflush(NULL);
    for (;;) {
        pi = __system_property_find(name);
        if (pi) break;
        long left = deadline - now_ms();
        if (left <= 0) { printf("timeout: %s never created\n", name); return 1; }
        struct timespec rel = { left / 1000, (left % 1000) * 1000000 };
        uint32_t ns;
        if (__system_property_wait(NULL, serial, &ns, &rel)) { serial = ns; wakeups++; }
    }
    for (;;) {
        struct value v = { "", 0 };
        __system_property_read_callback(pi, got, &v);
        if (!strcmp(v.v, want)) {
            printf("woke: %s=%s after %ld ms (%d futex wakeups)\n", name, v.v, now_ms() - t0, wakeups);
            return 0;
        }
        long left = deadline - now_ms();
        if (left <= 0) { printf("timeout: %s=%s\n", name, v.v); return 1; }
        struct timespec rel = { left / 1000, (left % 1000) * 1000000 };
        uint32_t ns;
        if (__system_property_wait(pi, v.serial, &ns, &rel)) wakeups++;
    }
}

// crtbegin.c's _start_main: the linker has run the init arrays of a dynamic
// executable already; __libc_init registers fini_array (its first entry
// must be -1) and calls main.
typedef struct {
    void (**preinit_array)(void);
    void (**init_array)(void);
    void (**fini_array)(void);
} structors_array_t;
__attribute__((noreturn)) void __libc_init(void *raw_args, void (*onexit)(void),
                                           int (*slingshot)(int, char **, char **),
                                           structors_array_t const *const structors);
static void (*fini_array[2])(void) = { (void (*)(void))-1, 0 };
__attribute__((used)) static void _start_main(void *raw_args)
{
    structors_array_t array = { 0, 0, fini_array };
    __libc_init(raw_args, 0, main, &array);
}
#if defined(__x86_64__)   // the x86_64 root, under FEX (tests/android/run.sh, x86_64 section)
__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  call _start_main\n  hlt\n");
#else
__asm__(".globl _start\n_start:\n  mov x0, sp\n  b _start_main\n");
#endif
