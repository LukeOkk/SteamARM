// ARM64_AUXV_LAYOUT: a 4 KiB-aligned DSO with writable data off its code host page.
int pg4k_answer(void) { return 42; }
int pg4k_counter __attribute__((aligned(32768)));
int pg4k_bump(void) { return ++pg4k_counter; }
