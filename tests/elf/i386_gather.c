// i386 probe: AVX2 gathers. FEX emulates them lane by lane; under a guest
// base each lane's address must get the base added (the Steam client and
// gldriverquery faulted forever on raw guest addresses).
// build-flags: -mavx2 -ffreestanding
#include <stdint.h>
#include <immintrin.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));

static int table[64];
static int check(void)
{
    for (int i = 0; i < 64; i++) table[i] = i * 3 + 1;
    __m128i idx4 = _mm_setr_epi32(1, 5, 9, 13);
    __m128i g4 = _mm_i32gather_epi32(table, idx4, 4);
    __m256i idx8 = _mm256_setr_epi32(0, 7, 14, 21, 28, 35, 42, 63);
    __m256i g8 = _mm256_i32gather_epi32(table, idx8, 4);
    __m256i mask = _mm256_setr_epi32(-1, 0, -1, 0, -1, 0, -1, 0);
    __m256i src = _mm256_set1_epi32(-7);
    __m256i gm = _mm256_mask_i32gather_epi32(src, table, idx8, mask, 4);
    int a[4], b[8], c[8];
    _mm_storeu_si128((__m128i *)a, g4);
    _mm256_storeu_si256((__m256i *)b, g8);
    _mm256_storeu_si256((__m256i *)c, gm);
    int ok = a[0] == 4 && a[1] == 16 && a[2] == 28 && a[3] == 40;
    const int want8[8] = {1, 22, 43, 64, 85, 106, 127, 190};
    for (int i = 0; i < 8; i++) {
        ok &= b[i] == want8[i];
        ok &= c[i] == ((i & 1) ? -7 : want8[i]);
    }
    printf("gather4 %d %d %d %d gather8[7] %d masked[1] %d masked[2] %d\n", a[0], a[1], a[2], a[3], b[7], c[1], c[2]);
    printf(ok ? "== gather: ok\n" : "== gather: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
