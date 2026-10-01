// Waydroid's audio HAL with a presentation position: x86_64 Android, loaded
// by audioserver as /vendor/lib64/hw/audio.primary.default.so (one bind for
// that process, scripts/android-boot.py "shims64"; ro.hardware.audio.primary
// is "default"). It opens the image's audio.primary.waydroid.so and changes
// only its output streams.
//
// Waydroid's HAL (alsa-lib to PulseAudio) never reports a presentation
// position: AudioFlinger counted "Timestamp stats: n=0 ... err=440",
// AudioTrack.getTimestamp() had nothing, and VLC's player paused itself
// 1.8 s into every video (benchmarks/stage35-android-apps-windows.txt).
// Android requires the call of a primary output. Here it is the frames
// the HAL has accepted minus its reported latency, at CLOCK_MONOTONIC now
// -- what AOSP's own reference HALs report without a hardware counter.
//
// Layouts are Android 11's hardware/hardware.h and hardware/audio.h (64-bit).
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t_;
struct ts_ { long tv_sec; long tv_nsec; };

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int clock_gettime(int, struct ts_ *);
extern int __android_log_print(int, const char *, const char *, ...);

#define RTLD_NOW_ 2
#define CLOCK_MONOTONIC_ 1
#define LOG_INFO_ 4

struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *, const char *, struct hw_device_t **);
};
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *);
};

// struct audio_stream: 14 function pointers, in this order.
enum { S_GET_SAMPLE_RATE, S_SET_SAMPLE_RATE, S_GET_BUFFER_SIZE, S_GET_CHANNELS, S_GET_FORMAT,
       S_SET_FORMAT, S_STANDBY, S_DUMP, S_GET_DEVICE, S_SET_DEVICE, S_SET_PARAMETERS,
       S_GET_PARAMETERS, S_ADD_EFFECT, S_REMOVE_EFFECT, S_COUNT };
struct audio_stream_out {
    void *common[S_COUNT];
    uint32_t (*get_latency)(const struct audio_stream_out *);
    int (*set_volume)(struct audio_stream_out *, float, float);
    ssize_t_ (*write)(struct audio_stream_out *, const void *, size_t);
    int (*get_render_position)(const struct audio_stream_out *, uint32_t *);
    int (*get_next_write_timestamp)(const struct audio_stream_out *, int64_t *);
    int (*set_callback)(struct audio_stream_out *, void *, void *);
    int (*pause)(struct audio_stream_out *);
    int (*resume)(struct audio_stream_out *);
    int (*drain)(struct audio_stream_out *, int);
    int (*flush)(struct audio_stream_out *);
    int (*get_presentation_position)(const struct audio_stream_out *, uint64_t *, struct ts_ *);
};
struct audio_hw_device {
    struct hw_device_t common;
    void *before_open_output[11];   // get_supported_devices .. get_input_buffer_size
    int (*open_output_stream)(struct audio_hw_device *, int, uint32_t, int, void *,
                              struct audio_stream_out **, const char *);
    void (*close_output_stream)(struct audio_hw_device *, struct audio_stream_out *);
};

enum { MAXS = 16 };
static struct {
    struct audio_stream_out *out;
    ssize_t_ (*write)(struct audio_stream_out *, const void *, size_t);
    uint32_t frame_size, rate;
    uint64_t written;               // frames the HAL accepted (atomic)
} g_s[MAXS];
static int (*g_open)(struct audio_hw_device *, int, uint32_t, int, void *, struct audio_stream_out **, const char *);
static void (*g_close)(struct audio_hw_device *, struct audio_stream_out *);

static int slot(const struct audio_stream_out *out)
{
    for (int i = 0; i < MAXS; i++)
        if (__atomic_load_n(&g_s[i].out, __ATOMIC_ACQUIRE) == out)
            return i;
    return -1;
}

static uint32_t bytes_per_sample(uint32_t format)
{
    switch (format) {
    case 0x1: return 2;             // AUDIO_FORMAT_PCM_16_BIT
    case 0x2: return 1;             // PCM_8_BIT
    case 0x3: case 0x4: case 0x5: return 4;   // PCM_32_BIT, PCM_8_24_BIT, PCM_FLOAT
    case 0x6: return 3;             // PCM_24_BIT_PACKED
    default: return 0;
    }
}

static ssize_t_ out_write(struct audio_stream_out *out, const void *buf, size_t bytes)
{
    int i = slot(out);
    if (i < 0)
        return -22;
    ssize_t_ n = g_s[i].write(out, buf, bytes);
    if (n > 0 && g_s[i].frame_size)
        __atomic_add_fetch(&g_s[i].written, (uint64_t)n / g_s[i].frame_size, __ATOMIC_RELAXED);
    return n;
}

static int out_presentation(const struct audio_stream_out *out, uint64_t *frames, struct ts_ *ts)
{
    int i = slot(out);
    if (i < 0 || !frames || !ts)
        return -22;
    uint64_t w = __atomic_load_n(&g_s[i].written, __ATOMIC_RELAXED);
    uint64_t lat = out->get_latency ? (uint64_t)out->get_latency(out) * g_s[i].rate / 1000 : 0;
    *frames = w > lat ? w - lat : 0;
    clock_gettime(CLOCK_MONOTONIC_, ts);
    return 0;
}

static int dev_open_output(struct audio_hw_device *dev, int handle, uint32_t devices, int flags, void *config,
                           struct audio_stream_out **outp, const char *address)
{
    int r = g_open(dev, handle, devices, flags, config, outp, address);
    if (r != 0 || !outp || !*outp)
        return r;
    struct audio_stream_out *out = *outp;
    uint32_t (*rate)(const void *) = (uint32_t (*)(const void *))out->common[S_GET_SAMPLE_RATE];
    uint32_t (*chan)(const void *) = (uint32_t (*)(const void *))out->common[S_GET_CHANNELS];
    uint32_t (*fmt)(const void *) = (uint32_t (*)(const void *))out->common[S_GET_FORMAT];
    uint32_t fs = bytes_per_sample(fmt ? fmt(out) : 0) * (uint32_t)__builtin_popcount(chan ? chan(out) : 0);
    for (int i = 0; i < MAXS; i++) {
        struct audio_stream_out *none = NULL;
        // Claim the slot first; out->write is redirected only once it is filled.
        if (__atomic_compare_exchange_n(&g_s[i].out, &none, out, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            g_s[i].write = out->write;
            g_s[i].frame_size = fs;
            g_s[i].rate = rate ? rate(out) : 48000;
            __atomic_store_n(&g_s[i].written, 0, __ATOMIC_RELEASE);
            out->write = out_write;
            out->get_presentation_position = out_presentation;
            __android_log_print(LOG_INFO_, "steamarm-audio",
                                "output %d: presentation position from %u-byte frames at %u Hz",
                                handle, fs, g_s[i].rate);
            break;
        }
    }
    return r;
}

static void dev_close_output(struct audio_hw_device *dev, struct audio_stream_out *out)
{
    int i = slot(out);
    if (i >= 0)
        __atomic_store_n(&g_s[i].out, NULL, __ATOMIC_RELEASE);
    g_close(dev, out);
}

static int module_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    (void)module;
    void *h = dlopen("/vendor/lib64/hw/audio.primary.waydroid.so", RTLD_NOW_);
    struct hw_module_t *orig = h ? (struct hw_module_t *)dlsym(h, "HMI") : NULL;
    if (!orig || !orig->methods || !orig->methods->open)
        return -19;                 // -ENODEV
    int r = orig->methods->open(orig, id, device);
    if (r != 0 || !device || !*device)
        return r;
    struct audio_hw_device *dev = (struct audio_hw_device *)*device;
    g_open = dev->open_output_stream;
    g_close = dev->close_output_stream;
    dev->open_output_stream = dev_open_output;
    dev->close_output_stream = dev_close_output;
    return 0;
}

static struct hw_module_methods_t g_methods = { module_open };

__attribute__((visibility("default"))) struct hw_module_t HMI = {
    .tag = ('H' << 24) | ('W' << 16) | ('M' << 8) | 'T',     // HARDWARE_MODULE_TAG
    .module_api_version = 0x0001,                            // AUDIO_MODULE_API_VERSION_0_1
    .hal_api_version = 0,
    .id = "audio",
    .name = "Waydroid audio HAL with a presentation position (SteamARM)",
    .author = "SteamARM",
    .methods = &g_methods,
};
