// The input path of Waydroid's composer, without the composer: its two ends
// as an x86-64 program linked against the image's bionic, run under FEX.
//
//   input_fifo write <name> <n>   as hwcomposer.waydroid (wayland-hwc.cpp):
//                                 mkfifo /dev/input/<name> if it is missing,
//                                 open it O_WRONLY|O_NONBLOCK (ENXIO until a
//                                 reader has it open: retried for 10 s), then
//                                 write n EV_REL/EV_KEY/EV_SYN records
//   input_fifo read <name> <n>    as Waydroid's EventHub (libinputreader.so
//                                 opens /dev/input/wl_*_events): open the FIFO
//                                 O_RDONLY|O_NONBLOCK, poll, read 24-byte
//                                 input_event records until n have come
//   input_fifo stat <name>        "present" or "absent"
//
// The runtime maps /dev/input to LXRT_INPUT_DIR when it is set, so two guests
// with the same value meet in the FIFO and a guest with another does not see
// it (runtime/evdev.c; benchmarks/stage28-android-reliability.txt).
#include <stdint.h>
#include <stddef.h>

typedef long ssize_t;
struct pollfd { int fd; short events, revents; };
struct timespec { long tv_sec, tv_nsec; };
struct input_event { long sec, usec; uint16_t type, code; int32_t value; };
int printf(const char *fmt, ...);
int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int mkfifo(const char *path, unsigned mode);
int poll(struct pollfd *fds, unsigned long n, int timeout);
int nanosleep(const struct timespec *req, struct timespec *rem);
int access(const char *path, int mode);
int atoi(const char *s);
int snprintf(char *b, size_t n, const char *fmt, ...);
int strcmp(const char *a, const char *b);
int *__errno(void);
#define errno (*__errno())
#define O_RDONLY 0
#define O_WRONLY 1
#define O_NONBLOCK 04000
#define ENXIO 6
#define EAGAIN 11
#define POLLIN 1

static void ms(long n) { struct timespec t = { n / 1000, (n % 1000) * 1000000 }; nanosleep(&t, 0); }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: input_fifo write|read|stat <name> [n]\n"); return 2; }
    char path[256];
    snprintf(path, sizeof path, "/dev/input/%s", argv[2]);
    int n = argc > 3 ? atoi(argv[3]) : 0;
    if (!strcmp(argv[1], "stat")) {
        printf("%s\n", access(path, 0) == 0 ? "present" : "absent");
        return 0;
    }
    if (!strcmp(argv[1], "write")) {
        if (access(path, 0) != 0 && mkfifo(path, 0666) != 0) {
            printf("mkfifo %s: errno %d\n", path, errno);
            return 1;
        }
        int fd = -1, enxio = 0;
        for (int i = 0; i < 1000 && fd < 0; i++) {
            fd = open(path, O_WRONLY | O_NONBLOCK);
            if (fd < 0 && errno == ENXIO) { enxio++; ms(10); }
            else if (fd < 0) { printf("open %s: errno %d\n", path, errno); return 1; }
        }
        if (fd < 0) { printf("no reader in 10 s (ENXIO x%d)\n", enxio); return 1; }
        printf("writer: open after %d ENXIO\n", enxio);
        for (int i = 0; i < n; i++) {
            struct input_event ev[3] = {
                { 0, 0, 2 /* EV_REL */, 0 /* REL_X */, i + 1 },
                { 0, 0, 1 /* EV_KEY */, 0x110 /* BTN_LEFT */, i & 1 },
                { 0, 0, 0 /* EV_SYN */, 0, 0 },
            };
            if (write(fd, ev, sizeof ev) != (ssize_t)sizeof ev) { printf("write: errno %d\n", errno); return 1; }
        }
        printf("writer: %d packets written\n", n);
        close(fd);
        return 0;
    }
    int fd = -1;
    for (int i = 0; i < 1000 && fd < 0; i++) {       // the FIFO may not exist yet
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) ms(10);
    }
    if (fd < 0) { printf("reader: cannot open %s: errno %d\n", path, errno); return 1; }
    printf("reader: open\n");
    int got = 0, rel = 0, keys = 0, abs = 0, syn = 0, sum = 0;
    for (int waits = 0; got < n * 3 && waits < 200;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) { waits++; continue; }
        struct input_event ev[16];
        ssize_t r = read(fd, ev, sizeof ev);
        if (r < 0 && errno == EAGAIN) continue;
        if (r <= 0) { waits++; ms(10); continue; }      // no writer (yet, or any more)
        for (ssize_t k = 0; k < r / (ssize_t)sizeof ev[0]; k++, got++) {
            if (got < 6)
                printf("reader: record %d: type %u code %u value %d\n", got, ev[k].type, ev[k].code, ev[k].value);
            if (ev[k].type == 0) syn++;
            if (ev[k].type == 1) keys++;
            if (ev[k].type == 2) { rel++; sum += ev[k].value; }
            if (ev[k].type == 3) abs++;
        }
    }
    printf("reader: %d records: %d EV_SYN, %d EV_KEY, %d EV_REL (sum %d), %d EV_ABS\n", got, syn, keys, rel, sum, abs);
    return got == n * 3 && sum == n * (n + 1) / 2 ? 0 : 1;
}

// crtbegin's part (tests/android/bionic_min.h).
typedef struct { void (**preinit_array)(void); void (**init_array)(void); void (**fini_array)(void); } structors_array_t;
__attribute__((noreturn)) void __libc_init(void *raw_args, void (*onexit)(void),
                                           int (*slingshot)(int, char **, char **),
                                           structors_array_t const *const structors);
static void (*fini[2])(void) = { (void (*)(void))-1, 0 };
static int slingshot(int argc, char **argv, char **envp) { (void)envp; return main(argc, argv); }
__attribute__((used)) static void _start_main(void *raw_args)
{
    structors_array_t array = { 0, 0, fini };
    __libc_init(raw_args, 0, slingshot, &array);
}
__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  call _start_main\n  hlt\n");
