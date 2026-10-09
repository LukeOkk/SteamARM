// An empty library to preload: it puts one more object ahead of libc in
// ld.so's lookup scope (tests/elf/fork_lazy_bind.c).
int preload_dummy_marker = 1;
