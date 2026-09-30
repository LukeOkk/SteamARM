// The few bionic declarations tests/android/binder_service.c needs, to link
// it against an Android image's own libc.so (there is no NDK here), and
// crtbegin's _start. The values are bionic's public ones (libc/include:
// errno.h, fcntl.h, stdio.h, sys/mman.h) and the same on x86_64 and aarch64.
//
//   clang --target=x86_64-linux-android30 -DBIONIC_MIN -nostdlib -fPIE -pie
//         -isystem <a sysroot with linux/android/binder.h>
//         -Wl,--dynamic-linker=/system/bin/linker64 binder_service.c <root>/.../libc.so
#ifndef STEAMARM_BIONIC_MIN_H
#define STEAMARM_BIONIC_MIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct __sFILE FILE;
typedef long ssize_t;
typedef long off_t;
extern FILE *stdout;
extern FILE *stderr;
int *__errno(void);
#define errno (*__errno())
#define EINTR 4
#define O_RDWR 02
#define O_CLOEXEC 02000000
#define PROT_READ 1
#define MAP_PRIVATE 2
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
#define _IOLBF 1
int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int ioctl(int fd, int req, ...);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
void perror(const char *s);
int fflush(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);

// crtbegin.c's _start_main: the linker has run the init arrays of a dynamic
// executable already; __libc_init registers fini_array (its first entry must
// be -1) and calls main.
typedef struct {
    void (**preinit_array)(void);
    void (**init_array)(void);
    void (**fini_array)(void);
} structors_array_t;
__attribute__((noreturn)) void __libc_init(void *raw_args, void (*onexit)(void),
                                           int (*slingshot)(int, char **, char **),
                                           structors_array_t const *const structors);
int main(int argc, char **argv);
static void (*bionic_min_fini[2])(void) = { (void (*)(void))-1, 0 };
static int bionic_min_main(int argc, char **argv, char **envp) { (void)envp; return main(argc, argv); }
__attribute__((used)) static void _start_main(void *raw_args)
{
    structors_array_t array = { 0, 0, bionic_min_fini };
    __libc_init(raw_args, 0, bionic_min_main, &array);
}
#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  call _start_main\n  hlt\n");
#elif defined(__aarch64__)
__asm__(".globl _start\n_start:\n  mov x0, sp\n  b _start_main\n");
#endif
#endif
