// offmap.c -- file mappings whose offset is on a 4 KiB guest page but not on
// a 16 KiB host page, and the host bytes around them that nobody can name.
#ifndef LXRT_OFFMAP_H
#define LXRT_OFFMAP_H

#include <stdint.h>

// dispatch.c mapped host pages [hstart, hend) and gave the guest
// [gstart, gstart+glen) inside them.
void lxrt_offmap_note(uint64_t hstart, uint64_t hend, uint64_t gstart, uint64_t glen);

// The guest unmapped [addr, addr+len) and the host pages wholly inside it are
// gone. Unmaps each partly covered host page at either end that this module
// owns and that no longer holds a byte the guest mapped. Writes those pages
// to dead[] (at most 2) and returns how many there are.
int lxrt_offmap_unmapped(uint64_t addr, uint64_t len, uint64_t dead[2]);

// Something else is about to live in [addr, addr+len) (MAP_FIXED, mremap,
// shmat): the guest bytes there are no longer the recorded mapping's, and the
// host pages touched are no longer this module's to unmap.
void lxrt_offmap_disown(uint64_t addr, uint64_t len);

#endif
