#ifndef LXRT_MREMAP_H
#define LXRT_MREMAP_H

#include <stdint.h>

// Linux aarch64 syscall 216. Returns an address or a negative Linux errno.
// Lengths are rounded to host pages; tracked sub-page ranges are unsupported.
// DONTUNMAP leaves fresh anonymous PROT_NONE pages at the old address.
// File-backed/shared growth or relocation and mixed-protection sources are
// unsupported. Callers serialize changes to overlapping address ranges.
long lxrt_mremap(uint64_t old_addr, uint64_t old_len,
                 uint64_t new_len, int lflags, uint64_t new_addr);

#endif
