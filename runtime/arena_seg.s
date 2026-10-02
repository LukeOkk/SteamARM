// The address space kept for the guest's fixed mappings (runtime/arena.c): a
// zero-fill segment with no access, so the kernel maps it when it loads the
// runtime -- before libmalloc, whose first regions land at a random place in
// this same part of the address space, has run a single instruction. It
// takes no room in the file. The Makefile gives it its address
// (-segaddr __LXRT_ARENA 0x13b000000) and protection (-segprot ... ---):
// the image slides by up to 0x5000000 (MEASURED, 1500 starts: 0x4000 to
// 0x4ff8000), so 0x140000000 - 0x160000000 is inside it at any slide, and
// its end stays below the main thread's stack, which slides down by the
// same amount (0x16be00000 - slide).
.zerofill __LXRT_ARENA,__arena,_lxrt_arena_seg,0x25000000,14
