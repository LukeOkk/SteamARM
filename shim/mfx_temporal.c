// vkCmdUpdateBuffer: the command channel of a temporal upscale handed to
// MetalFX (shim/mfx_temporal.h has the command and why it travels this way).
//
// An update of exactly sizeof(struct sa_mfxt_cmd) bytes whose data starts
// with SA_MFXT_MAGIC and SA_MFXT_VERSION is a command for this shim, not a
// buffer update: it goes to the driver's kk_steamarm_upscale_temporal when
// the driver has one, its status (and, for PROBE, the caps the driver
// reports) is written back into the sender's data
// before this returns, and it is NOT forwarded (the carrier buffer stays as
// it was). Without that entry the status is SA_ST_NO_DRIVER, and the sender
// falls back to the game's own upscaler. Every other update goes to the
// driver unchanged.
//
// pData has already been moved out of a guest's low window by the generated
// wrapper (build/vk_rebase.c): an address change on the same memory, so the
// status lands in the sender's copy.
#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan_core.h>
#include "lxrt_host.h"
#include "mfx_temporal.h"

extern int dprintf(int, const char *, ...);
extern char *getenv(const char *);
extern const char *lxrt_vk_driver;   // vulkan_shim.c (generated): the driver in use
extern void *lxrt_vk_driver_handle;  // vulkan_shim.c (generated): its host handle

void lxrt_mvk_vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void *);

typedef uint32_t (*kk_upscale_fn)(VkCommandBuffer, const struct sa_mfxt_cmd *);

static int debug_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_DEBUG");
        on = e && *e == '1';
    }
    return on;
}

// The driver's entry, looked up once: 0 = not yet, 1 = none, 2 = g_entry.
static int g_entry_state;
static kk_upscale_fn g_entry;

static kk_upscale_fn driver_entry(void)
{
    int st = __atomic_load_n(&g_entry_state, __ATOMIC_ACQUIRE);
    if (st == 0) {
        void *h = lxrt_vk_driver_handle;
        kk_upscale_fn f = h ? (kk_upscale_fn)lxrt_host_dlsym(h, SA_MFXT_KK_ENTRY) : 0;
        g_entry = f;
        st = f ? 2 : 1;
        __atomic_store_n(&g_entry_state, st, __ATOMIC_RELEASE);
    }
    return st == 2 ? g_entry : 0;
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case SA_OP_PROBE: return "probe";
    case SA_OP_ENCODE: return "encode";
    case SA_OP_DESTROY: return "destroy";
    default: return "unknown";
    }
}

static const char *status_name(uint32_t st)
{
    switch (st) {
    case SA_ST_UNSEEN: return "UNSEEN";
    case SA_ST_OK: return "OK";
    case SA_ST_NO_DRIVER: return "NO_DRIVER";
    case SA_ST_BAD_ARGS: return "BAD_ARGS";
    case SA_ST_UNSUPPORTED: return "UNSUPPORTED";
    case SA_ST_NO_SCALER: return "NO_SCALER";
    default: return "?";
    }
}

// One log line per context id, the first time it is seen (LXRT_VK_DEBUG=1).
// A context seen when the table is full is logged every time: rare, and
// better than silence.
enum { N_SEEN = 64 };
static uint64_t g_seen[N_SEEN];
static int g_seen_used[N_SEEN];   // 0 free, 1 being written, 2 holds g_seen[i]

static int first_sight(uint64_t ctx)
{
    for (int i = 0; i < N_SEEN; i++) {
        int u = __atomic_load_n(&g_seen_used[i], __ATOMIC_ACQUIRE);
        if (u == 0) {
            int expect = 0;
            if (__atomic_compare_exchange_n(&g_seen_used[i], &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                g_seen[i] = ctx;
                __atomic_store_n(&g_seen_used[i], 2, __ATOMIC_RELEASE);
                return 1;
            }
            u = expect;
        }
        while (u == 1)
            u = __atomic_load_n(&g_seen_used[i], __ATOMIC_ACQUIRE);
        if (g_seen[i] == ctx)
            return 0;
    }
    return 1;
}

static int is_command(VkDeviceSize size, const void *data)
{
    if (size != sizeof(struct sa_mfxt_cmd) || !data)
        return 0;
    uint64_t magic;
    uint32_t version;
    __builtin_memcpy(&magic, data, sizeof magic);
    __builtin_memcpy(&version, (const char *)data + 8, sizeof version);
    return magic == SA_MFXT_MAGIC && version == SA_MFXT_VERSION;
}

__attribute__((visibility("hidden")))
void lxrt_inner_vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
                                  VkDeviceSize dataSize, const void *pData)
{
    if (!is_command(dataSize, pData)) {
        lxrt_mvk_vkCmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
        return;
    }
    // An aligned copy: the sender's data need not be 8-byte aligned.
    struct sa_mfxt_cmd cmd;
    __builtin_memcpy(&cmd, pData, sizeof cmd);
    uint32_t st;
    kk_upscale_fn f = driver_entry();
    if (cmd.size != sizeof cmd || cmd.op < SA_OP_PROBE || cmd.op > SA_OP_DESTROY)
        st = SA_ST_BAD_ARGS;
    else if (!f)
        st = SA_ST_NO_DRIVER;
    else
        st = f(commandBuffer, &cmd);
    __builtin_memcpy((char *)pData + SA_MFXT_STATUS_OFFSET, &st, sizeof st);
    // PROBE's answer (caps, driver, largest scale) goes back with the status.
    if (cmd.op == SA_OP_PROBE && f)
        __builtin_memcpy((char *)pData + SA_MFXT_CAPS_OFFSET, (const char *)&cmd + SA_MFXT_CAPS_OFFSET, 12);
    if (debug_on() && first_sight(cmd.context))
        dprintf(2, "[shim] mfx-temporal ctx %llu op %s %ux%u->%ux%u fmt %u driver %s entry %s st=%s\n",
                (unsigned long long)cmd.context, op_name(cmd.op), cmd.render_w, cmd.render_h,
                cmd.upscale_w, cmd.upscale_h, cmd.color.vk_format, lxrt_vk_driver,
                f ? "found" : "missing", status_name(st));
}
