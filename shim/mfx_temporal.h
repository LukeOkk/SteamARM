// The hand-off of one temporal upscale (FSR 3.1 / DLSS style) to Apple's
// MetalFX, shared by the Windows FFX DLL that sends it, the Vulkan shim that
// catches it (shim/mfx_temporal.c) and the KosmicKrisp entry that encodes it
// (kk_steamarm_upscale_temporal).
//
// The sender records an ordinary vkCmdUpdateBuffer(cb, carrier, 0,
// sizeof(struct sa_mfxt_cmd), &cmd) into the game's command buffer, aimed at
// a real 4 KiB carrier buffer of its own. That call crosses winevulkan, the
// FEX thunks (its data pointer raw) and the shim's rebase wrapper unchanged.
// The shim recognises it by its size, magic and version, writes `status`
// back into the sender's copy before the call returns, and does not forward
// it; any other update goes to the driver as it came. A shim or driver that
// knows nothing of this just runs a harmless buffer update, and the sender
// sees status SA_ST_UNSEEN: it then falls back to the game's own upscaler in
// the same frame.
//
// Fixed layout, the same for every compiler that builds it (mingw x86_64,
// clang aarch64 Linux, clang macOS): only fixed-width fields, no pointers.
// Image handles are the driver's own VkImage values (winevulkan does not
// wrap VkImage). image 0 = absent.
#ifndef SA_MFX_TEMPORAL_H
#define SA_MFX_TEMPORAL_H

#include <stdint.h>

#define SA_MFXT_MAGIC 0x5458464D41455453ull   /* "STEAMFXT" */
#define SA_MFXT_VERSION 1u

enum { SA_OP_PROBE=1, SA_OP_ENCODE=2, SA_OP_DESTROY=3 };
enum { SA_ST_UNSEEN=0, SA_ST_OK=1, SA_ST_NO_DRIVER, SA_ST_BAD_ARGS, SA_ST_UNSUPPORTED, SA_ST_NO_SCALER };

/* sa_mfxt_cmd.flags */
#define SA_MFXT_RESET        1u
#define SA_MFXT_DEPTH_INV    2u
#define SA_MFXT_AUTO_EXP     4u
#define SA_MFXT_MV_DISPLAY   8u
#define SA_MFXT_MV_JITTERED 16u
#define SA_MFXT_HDR         32u
#define SA_MFXT_SHARPEN     64u

struct sa_mfxt_tex { uint64_t image; uint32_t vk_format, w, h, x, y, flags; };      /* 32 B */
struct sa_mfxt_cmd {
  uint64_t magic; uint32_t version, size, op, status /* written back */; uint64_t context;
  struct sa_mfxt_tex color, depth, motion, exposure, reactive, output;             /* image 0 = absent */
  uint32_t render_w, render_h, upscale_w, upscale_h;
  float jitter_x, jitter_y, mv_scale_x, mv_scale_y, pre_exposure, sharpness;
  uint32_t flags;   /* RESET 1, DEPTH_INV 2, AUTO_EXP 4, MV_DISPLAY 8, MV_JITTERED 16, HDR 32, SHARPEN 64 */
  uint32_t caps_out, driver_out; float max_scale_out; uint32_t reserved[10];       /* pad to 320 */
};

#define SA_MFXT_STATUS_OFFSET 20u   /* offsetof(struct sa_mfxt_cmd, status) */
#define SA_MFXT_CAPS_OFFSET 268u    /* caps_out, driver_out, max_scale_out: 12 bytes PROBE fills */

/* PROBE's caps_out bits, as KosmicKrisp writes them (kk_mfx_temporal.h). */
#define SA_MFXT_CAP_TEMPORAL    1u  /* MetalFX's temporal scaler (Metal 4) on this GPU */
#define SA_MFXT_CAP_MV_DISPLAY  2u  /* motion vectors at the output's size (macOS 27) */
#define SA_MFXT_CAP_MV_JITTERED 4u  /* motion vectors with the jitter in them (macOS 27) */
#define SA_MFXT_CAP_OFFSETS     8u  /* sa_mfxt_tex.x/y: content not at the origin (macOS 27) */
#define SA_MFXT_CAP_REACTIVE   16u  /* a reactive mask */
#define SA_MFXT_CAP_SCRATCH    32u  /* the output goes through a private texture and a copy */

_Static_assert(sizeof(struct sa_mfxt_tex) == 32, "sa_mfxt_tex is 32 bytes");
_Static_assert(sizeof(struct sa_mfxt_cmd) == 320, "sa_mfxt_cmd is 320 bytes");
_Static_assert(__builtin_offsetof(struct sa_mfxt_cmd, status) == SA_MFXT_STATUS_OFFSET, "status at 20");
_Static_assert(__builtin_offsetof(struct sa_mfxt_cmd, color) == 32, "textures at 32");
_Static_assert(__builtin_offsetof(struct sa_mfxt_cmd, render_w) == 224, "sizes at 224");
_Static_assert(__builtin_offsetof(struct sa_mfxt_cmd, caps_out) == SA_MFXT_CAPS_OFFSET, "caps at 268");

/* The KosmicKrisp entry (default visibility in the driver), found by the
   shim with lxrt_host_dlsym on the driver's handle; returns an SA_ST_*. */
#define SA_MFXT_KK_ENTRY "kk_steamarm_upscale_temporal"

#endif
