// A 4 KiB-aligned DSO whose code writes its own data from the 16 KiB host
// page that holds both (runtime/storemu.c). Built with -march=armv8-a
// -mno-outline-atomics, so the plain atomics are LDAXR/STLXR loops; the
// "+lse" ones are single LSE instructions.
int self4k_counter;
long self4k_wide[4];
int self4k_atomic;

int self4k_bump(void) { return ++self4k_counter; }

long self4k_fill(long v)
{
    for (int i = 0; i < 4; i++)
        self4k_wide[i] = v + i;
    return self4k_wide[3];
}

int self4k_llsc_add(int v) { return __atomic_add_fetch(&self4k_atomic, v, __ATOMIC_SEQ_CST); }

int self4k_llsc_cas(int from, int to)
{
    int e = from;
    __atomic_compare_exchange_n(&self4k_atomic, &e, to, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;
}

__attribute__((target("+lse"))) int self4k_lse_add(int v)
{
    return __atomic_add_fetch(&self4k_atomic, v, __ATOMIC_SEQ_CST);
}

__attribute__((target("+lse"))) int self4k_lse_cas(int from, int to)
{
    int e = from;
    __atomic_compare_exchange_n(&self4k_atomic, &e, to, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;
}
