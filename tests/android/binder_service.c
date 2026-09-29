// A native binder service that registers with Android 11's own
// servicemanager and answers Android's own `service` client -- raw ioctls,
// no libbinder, built for Linux aarch64 with glibc (there is no NDK here).
// It speaks the Android 11 parcel format by hand:
//
//   interface token  i32 strict-mode policy, i32 work source, i32 'SYST',
//                    String16 name (LineageOS 18.1's libbinder carries the
//                    'SYST' header Android 12 introduced: MEASURED, its Parcel
//                    logs "Expecting header 0x53595354" without it)
//   String16         i32 length, UTF-16 units + NUL, padded to 4 bytes
//   strong binder    flat_binder_object, then i32 stability (Android 11)
//   AIDL reply       i32 exception code (0: none), then the return value
//
// Registers as "steamarm.test" (IServiceManager::addService, code 3) and
// serves, as interface "steamarm.test.IEcho":
//   1  i32 N           -> N + 1
//   2  fd F            -> the number of bytes read from F, and their sum
//   3                  -> replies, then exits (servicemanager sees it die)
// plus INTERFACE_TRANSACTION and PING_TRANSACTION, as BBinder would.
//
//   binder_service [name]      prints "registered <name>" once added
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define B_PACK(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define FIRST_CALL_TRANSACTION 1
#define PING_TRANSACTION      B_PACK('_', 'P', 'N', 'G')
#define INTERFACE_TRANSACTION B_PACK('_', 'N', 'T', 'F')
#define UNKNOWN_TRANSACTION   (-74)
#define STABILITY_SYSTEM      12        // internal::Stability::SYSTEM (0b001100)
#define PARCEL_HEADER_SYST    B_PACK('S', 'Y', 'S', 'T')
#define DUMP_FLAG_PRIORITY_DEFAULT 8
#define SVC_PTR    0x5000
#define SVC_COOKIE 0x6000
static const char *DESCRIPTOR = "steamarm.test.IEcho";

static int g_fd;

struct parcel {
    uint8_t d[4096];
    size_t n;
    uint64_t offs[16];
    size_t noffs;
};
static void p_i32(struct parcel *p, int32_t v) { memcpy(p->d + p->n, &v, 4); p->n += 4; }
static void p_str16(struct parcel *p, const char *s)
{
    size_t len = strlen(s);
    p_i32(p, (int32_t)len);
    for (size_t i = 0; i <= len; i++) {
        uint16_t c = (uint8_t)s[i];
        memcpy(p->d + p->n, &c, 2);
        p->n += 2;
    }
    while (p->n & 3) p->d[p->n++] = 0;
}
static void p_token(struct parcel *p, const char *iface)
{
    p_i32(p, 0);            // strict-mode policy
    p_i32(p, -1);           // IPCThreadState::kUnsetWorkSource
    p_i32(p, PARCEL_HEADER_SYST);
    p_str16(p, iface);
}
static void p_binder(struct parcel *p, uint64_t ptr, uint64_t cookie)
{
    struct flat_binder_object o;
    memset(&o, 0, sizeof o);
    o.hdr.type = BINDER_TYPE_BINDER;
    o.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS | 0x13;
    o.binder = ptr;
    o.cookie = cookie;
    p->offs[p->noffs++] = p->n;
    memcpy(p->d + p->n, &o, sizeof o);
    p->n += sizeof o;
    p_i32(p, STABILITY_SYSTEM);
}

struct reader { const uint8_t *d; size_t n, pos; };
static int32_t r_i32(struct reader *r)
{
    int32_t v = 0;
    if (r->pos + 4 <= r->n) memcpy(&v, r->d + r->pos, 4);
    r->pos += 4;
    return v;
}
static void r_skip_str16(struct reader *r)
{
    int32_t len = r_i32(r);
    if (len < 0) return;
    r->pos += ((size_t)len + 1) * 2;
    r->pos = (r->pos + 3) & ~(size_t)3;
}

struct wbuf { uint8_t b[8192]; size_t n; };
static void w32(struct wbuf *w, uint32_t v) { memcpy(w->b + w->n, &v, 4); w->n += 4; }
static void w64(struct wbuf *w, uint64_t v) { memcpy(w->b + w->n, &v, 8); w->n += 8; }
static void wraw(struct wbuf *w, const void *p, size_t n) { memcpy(w->b + w->n, p, n); w->n += n; }

static int bwr(const void *wp, size_t wn, void *rp, size_t rn, size_t *got)
{
    struct binder_write_read b = { .write_size = wn, .write_buffer = (uintptr_t)wp,
                                   .read_size = rn, .read_buffer = (uintptr_t)rp };
    int r;
    do r = ioctl(g_fd, BINDER_WRITE_READ, &b);
    while (r < 0 && errno == EINTR);
    if (got) *got = b.read_consumed;
    return r;
}

static void put_reply(struct wbuf *w, struct parcel *p, bool status_only, int32_t status)
{
    static struct parcel keep;          // must outlive the write
    keep = *p;
    struct binder_transaction_data t;
    memset(&t, 0, sizeof t);
    if (status_only) {
        keep.n = 0;
        p_i32(&keep, status);
        t.flags = TF_STATUS_CODE;
    }
    t.data_size = keep.n;
    t.offsets_size = keep.noffs * 8;
    t.data.ptr.buffer = (uintptr_t)keep.d;
    t.data.ptr.offsets = (uintptr_t)keep.offs;
    w32(w, BC_REPLY);
    wraw(w, &t, sizeof t);
}

// Handle one read's worth of commands. *reply_status: set when a BR_REPLY
// (or failure) for our own call arrives.
static int handle(const uint8_t *rb, size_t got, int *reply_status, bool *quit)
{
    struct wbuf out = { .n = 0 };
    for (size_t q = 0; q + 4 <= got; ) {
        uint32_t cmd;
        memcpy(&cmd, rb + q, 4);
        q += 4;
        switch (cmd) {
        case BR_NOOP: case BR_TRANSACTION_COMPLETE: case BR_SPAWN_LOOPER:
            break;
        case BR_INCREFS: case BR_ACQUIRE: case BR_RELEASE: case BR_DECREFS: {
            struct binder_ptr_cookie pc;
            memcpy(&pc, rb + q, sizeof pc);
            q += sizeof pc;
            if (cmd == BR_INCREFS) { w32(&out, BC_INCREFS_DONE); wraw(&out, &pc, sizeof pc); }
            if (cmd == BR_ACQUIRE) { w32(&out, BC_ACQUIRE_DONE); wraw(&out, &pc, sizeof pc); }
            break;
        }
        case BR_REPLY: {
            struct binder_transaction_data t;
            memcpy(&t, rb + q, sizeof t);
            q += sizeof t;
            struct reader r = { (const uint8_t *)(uintptr_t)t.data.ptr.buffer, t.data_size, 0 };
            if (reply_status)
                *reply_status = (t.flags & TF_STATUS_CODE) ? r_i32(&r) : (t.data_size >= 4 ? r_i32(&r) : 0);
            w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
            break;
        }
        case BR_DEAD_REPLY: case BR_FAILED_REPLY:
            if (reply_status) *reply_status = -32;      // DEAD_OBJECT
            break;
        case BR_DEAD_BINDER: case BR_CLEAR_DEATH_NOTIFICATION_DONE:
            q += 8;
            break;
        case BR_TRANSACTION: case BR_TRANSACTION_SEC_CTX: {
            struct binder_transaction_data t;
            memcpy(&t, rb + q, sizeof t);
            q += cmd == BR_TRANSACTION ? sizeof t : sizeof(struct binder_transaction_data_secctx);
            struct reader r = { (const uint8_t *)(uintptr_t)t.data.ptr.buffer, t.data_size, 0 };
            const uint64_t *offs = (const uint64_t *)(uintptr_t)t.data.ptr.offsets;
            struct parcel rep = { .n = 0 };
            bool oneway = t.flags & TF_ONE_WAY;
            int fd_to_close = -1;
            if (t.code == INTERFACE_TRANSACTION) {
                p_str16(&rep, DESCRIPTOR);
                put_reply(&out, &rep, false, 0);
            } else if (t.code == PING_TRANSACTION) {
                put_reply(&out, &rep, false, 0);
            } else if (t.code == FIRST_CALL_TRANSACTION) {
                r_i32(&r); r_i32(&r); r_i32(&r); r_skip_str16(&r);   // interface token
                int32_t v = r_i32(&r);
                p_i32(&rep, 0);
                p_i32(&rep, v + 1);
                printf("call 1: %d -> %d (from pid %d)\n", v, v + 1, t.sender_pid);
                put_reply(&out, &rep, false, 0);
            } else if (t.code == FIRST_CALL_TRANSACTION + 1 && t.offsets_size >= 8) {
                struct flat_binder_object o;
                memcpy(&o, r.d + offs[0], sizeof o);
                int32_t count = -1, sum = 0;
                if (o.hdr.type == BINDER_TYPE_FD) {
                    int fd = (int)o.handle;
                    uint8_t buf[4096];
                    ssize_t k;
                    count = 0;
                    while ((k = read(fd, buf, sizeof buf)) > 0) {
                        count += (int32_t)k;
                        for (ssize_t i = 0; i < k; i++) sum += buf[i];
                    }
                    fd_to_close = fd;
                }
                p_i32(&rep, 0);
                p_i32(&rep, count);
                p_i32(&rep, sum);
                printf("call 2: read %d bytes (sum %d) from a descriptor\n", count, sum);
                put_reply(&out, &rep, false, 0);
            } else if (t.code == FIRST_CALL_TRANSACTION + 2) {
                p_i32(&rep, 0);
                put_reply(&out, &rep, false, 0);
                printf("call 3: exiting\n");
                *quit = true;
            } else if (!oneway) {
                put_reply(&out, &rep, true, UNKNOWN_TRANSACTION);
            }
            if (fd_to_close >= 0) close(fd_to_close);     // Parcel::closeFileDescriptors
            w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
            if (oneway) {}          // no reply
            fflush(stdout);
            break;
        }
        default:
            fprintf(stderr, "binder_service: unexpected command 0x%x\n", cmd);
            return -1;
        }
    }
    if (out.n && bwr(out.b, out.n, NULL, 0, NULL) < 0) { perror("write"); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "steamarm.test";
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) { perror("open /dev/binder"); return 1; }
    struct binder_version v;
    if (ioctl(g_fd, BINDER_VERSION, &v) < 0 || v.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
        fprintf(stderr, "binder version mismatch\n");
        return 1;
    }
    if (mmap(NULL, (1 << 20) - 2 * 4096, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, g_fd, 0) == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    uint32_t zero = 0;
    ioctl(g_fd, BINDER_SET_MAX_THREADS, &zero);

    // IServiceManager::addService(name, binder, allowIsolated=false, DEFAULT)
    static struct parcel p;
    p_token(&p, "android.os.IServiceManager");
    p_str16(&p, name);
    p_binder(&p, SVC_PTR, SVC_COOKIE);
    p_i32(&p, 0);
    p_i32(&p, DUMP_FLAG_PRIORITY_DEFAULT);
    struct binder_transaction_data t;
    memset(&t, 0, sizeof t);
    t.target.handle = 0;
    t.code = FIRST_CALL_TRANSACTION + 2;
    t.flags = TF_ACCEPT_FDS;
    t.data_size = p.n;
    t.offsets_size = p.noffs * 8;
    t.data.ptr.buffer = (uintptr_t)p.d;
    t.data.ptr.offsets = (uintptr_t)p.offs;
    struct wbuf w = { .n = 0 };
    w32(&w, BC_TRANSACTION);
    wraw(&w, &t, sizeof t);
    int status = 1;
    bool quit = false;
    const void *wp = w.b;
    size_t wn = w.n;
    while (status == 1) {
        uint8_t rb[1024];
        size_t got = 0;
        if (bwr(wp, wn, rb, sizeof rb, &got) < 0) { perror("addService"); return 1; }
        wp = NULL; wn = 0;
        if (handle(rb, got, &status, &quit) < 0) return 1;
    }
    if (status != 0) {
        fprintf(stderr, "addService(%s) failed: %d\n", name, status);
        return 1;
    }
    printf("registered %s\n", name);

    // The looper: this thread serves calls until told to quit.
    w.n = 0;
    w32(&w, BC_ENTER_LOOPER);
    bwr(w.b, w.n, NULL, 0, NULL);
    while (!quit) {
        uint8_t rb[1024];
        size_t got = 0;
        if (bwr(NULL, 0, rb, sizeof rb, &got) < 0) { perror("read"); return 1; }
        if (handle(rb, got, NULL, &quit) < 0) return 1;
    }
    return 0;
}
