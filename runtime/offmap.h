// offmap.c -- the 4 KiB slots of host pages that no guest mapping covers
// (the head and tail of a mapping the runtime placed, what the guest unmapped
// of a host page that stays mapped), so munmap can free a host page with its
// last guest page.
#ifndef LXRT_OFFMAP_H
#define LXRT_OFFMAP_H

#include <stdint.h>

// dispatch.c mapped host pages [hstart, hend) and gave the guest
// [gstart, gstart+glen) inside them; the rest of those pages is nobody's.
void lxrt_offmap_note(uint64_t hstart, uint64_t hend, uint64_t gstart, uint64_t glen);

// The guest unmapped [addr, addr+len) (len rounded up to 4 KiB, as Linux
// does) and the host pages wholly inside it are gone. Records what it
// unmapped of a partly covered host page at either end, and unmaps each such
// page with no guest page left in it. Writes those pages to dead[] (at most
// 2) and returns how many there are.
int lxrt_offmap_unmapped(uint64_t addr, uint64_t len, uint64_t dead[2]);

// Something else is about to live in [addr, addr+len) (MAP_FIXED,
// MAP_FIXED_NOREPLACE): those 4 KiB slots are no longer nobody's.
void lxrt_offmap_disown(uint64_t addr, uint64_t len);

// The same for every slot of the host pages [addr, addr+len) touches: the
// host pages themselves were moved or replaced (mremap, shmat, binder).
void lxrt_offmap_disown_pages(uint64_t addr, uint64_t len);

#endif
