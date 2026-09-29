#ifndef LXRT_MREMAP_H
#define LXRT_MREMAP_H

#include <stdbool.h>
#include <stdint.h>

// Linux aarch64 syscall 216. Returns an address or a negative Linux errno.
// Lengths are rounded to host pages; tracked sub-page ranges are unsupported.
// DONTUNMAP leaves fresh anonymous PROT_NONE pages at the old address.
// File-backed/shared growth or relocation and mixed-protection sources are
// unsupported. Callers serialize changes to overlapping address ranges.
long lxrt_mremap(uint64_t old_addr, uint64_t old_len,
                 uint64_t new_len, int lflags, uint64_t new_addr);

// For dispatch.c's sub-page mremap (4 KiB guest pages inside 16 KiB host
// pages, which lxrt_mremap refuses): copy len bytes at src, whatever its
// protection, into dst (0, or -1 when part of the source is unmapped); and
// the protection the kernel gives the host page at addr (-1: unmapped),
// and whether it is shared memory.
int lxrt_mremap_copy_out(void *dst, uint64_t src, uint64_t len);
int lxrt_mremap_host_prot(uint64_t addr, bool *shared);

#endif
