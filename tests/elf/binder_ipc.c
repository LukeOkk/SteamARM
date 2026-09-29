// Binder between two lxrun processes, with raw ioctls and no libbinder
// (runtime/binder.c, runtime/binder_hub.c). Built against the Linux UAPI
// header <linux/android/binder.h>, so every number here is the kernel's,
// not the runtime's own copy of it.
//
// The parent is the context manager (servicemanager's role); a forked child
// is the client. Checked, each with an "ok" line:
//   version, mmap rules, context manager, sync transaction + reply with the
//   sender's pid, a file descriptor transfer (BINDER_TYPE_FD), a binder
//   object transfer with the node reference protocol (BR_INCREFS/BR_ACQUIRE
//   and their DONEs), a nested call back into the client during its own
//   call (the transaction-stack thread selection), oneway transactions
//   serialised per node, scatter-gather (BINDER_TYPE_PTR with a parent
//   fixup) with a descriptor array (BINDER_TYPE_FDA, closed when the buffer
//   is freed), errors (invalid handle, context manager calling itself),
//   poll readiness with no spurious wakeup, and a death notification when
//   the client exits.
//
//   binder_ipc            the test (exit 0 and "== binder ipc: ok")
//   binder_ipc bench N    N sync round trips, timing only
//   binder_ipc pool [N]   the thread pool (see pool_test); N: also time N
//                         round trips to a parked looper
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAP_SZ ((1 << 20) - 2 * 4096)
static int g_fail;
#define CHECK(c, ...) do { if (c) { printf("  ok    " __VA_ARGS__); printf("\n"); } \
                           else { printf("  MAL   " __VA_ARGS__); printf("   (%s:%d, errno %d)\n", __FILE__, __LINE__, errno); g_fail++; } \
                           fflush(stdout); } while (0)

enum { C_PING = 1, C_FD, C_REGISTER, C_CALLBACK, C_ONEWAY, C_COUNT, C_SG, C_CLEAR, C_QUIT = 99, C_CB = 10 };
#define CLIENT_PTR    0x1000
#define CLIENT_COOKIE 0x2000
#define DEATH_COOKIE  0xdead

struct wb { uint8_t b[4096]; size_t n; };
static void w32(struct wb *w, uint32_t v) { memcpy(w->b + w->n, &v, 4); w->n += 4; }
static void w64(struct wb *w, uint64_t v) { memcpy(w->b + w->n, &v, 8); w->n += 8; }
static void wbytes(struct wb *w, const void *p, size_t n) { memcpy(w->b + w->n, p, n); w->n += n; }

static volatile int g_eintr;
static int wr(int fd, const void *w, size_t wn, void *r, size_t rn, size_t *got)
{
    struct binder_write_read b = { .write_size = wn, .write_buffer = (uintptr_t)w,
                                   .read_size = rn, .read_buffer = (uintptr_t)r };
    int ret;
    do {
        ret = ioctl(fd, BINDER_WRITE_READ, &b);
        if (ret < 0 && errno == EINTR) g_eintr++;
    } while (ret < 0 && errno == EINTR);    // consumed counts carry the restart
    if (got) *got = b.read_consumed;
    return ret;
}

static int open_binder(const char *what)
{
    int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("  MAL   %s: open /dev/binder: %s\n", what, strerror(errno)); exit(2); }
    return fd;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ------------------------------------------------------------------ client

static int c_fd;
static int c_cb_calls;

// Send one transaction, wait for its reply (answering whatever arrives in
// between). Returns 0 and the reply's payload, or the BR_ error code.
static uint32_t transact(uint32_t handle, uint32_t code, uint32_t flags, const void *data, size_t dn,
                         const uint64_t *offs, size_t on, uint64_t sg_size,
                         uint8_t *reply, size_t *reply_n)
{
    struct wb w = { .n = 0 };
    struct binder_transaction_data_sg trs;
    memset(&trs, 0, sizeof trs);
    struct binder_transaction_data *tr = &trs.transaction_data;
    tr->target.handle = handle;
    tr->code = code;
    tr->flags = flags | TF_ACCEPT_FDS;
    tr->data_size = dn;
    tr->offsets_size = on * 8;
    tr->data.ptr.buffer = (uintptr_t)data;
    tr->data.ptr.offsets = (uintptr_t)offs;
    if (sg_size) {
        trs.buffers_size = sg_size;
        w32(&w, BC_TRANSACTION_SG);
        wbytes(&w, &trs, sizeof trs);
    } else {
        w32(&w, BC_TRANSACTION);
        wbytes(&w, tr, sizeof *tr);
    }
    bool oneway = flags & TF_ONE_WAY;
    const void *wp = w.b;
    size_t wn = w.n;
    for (;;) {
        uint8_t rb[512];
        size_t got = 0;
        if (wr(c_fd, wp, wn, rb, sizeof rb, &got) < 0) { perror("client BINDER_WRITE_READ"); return 1; }
        wp = NULL; wn = 0;
        struct wb out = { .n = 0 };
        uint32_t result = 0xffffffff;
        for (size_t p = 0; p + 4 <= got; ) {
            uint32_t cmd;
            memcpy(&cmd, rb + p, 4);
            p += 4;
            switch (cmd) {
            case BR_NOOP: case BR_SPAWN_LOOPER: break;
            case BR_TRANSACTION_COMPLETE:
                if (oneway) result = 0;
                break;
            case BR_INCREFS: case BR_ACQUIRE: case BR_RELEASE: case BR_DECREFS: {
                struct binder_ptr_cookie pc;
                memcpy(&pc, rb + p, sizeof pc);
                p += sizeof pc;
                if (cmd == BR_INCREFS) { w32(&out, BC_INCREFS_DONE); wbytes(&out, &pc, sizeof pc); }
                if (cmd == BR_ACQUIRE) { w32(&out, BC_ACQUIRE_DONE); wbytes(&out, &pc, sizeof pc); }
                if (cmd == BR_ACQUIRE && pc.ptr == CLIENT_PTR && pc.cookie == CLIENT_COOKIE)
                    printf("  (client: BR_ACQUIRE for its node)\n");
                break;
            }
            case BR_TRANSACTION: {
                // The server calling back into us while we wait (C_CALLBACK).
                struct binder_transaction_data t;
                memcpy(&t, rb + p, sizeof t);
                p += sizeof t;
                uint32_t v = 0;
                if (t.data_size >= 4) memcpy(&v, (void *)(uintptr_t)t.data.ptr.buffer, 4);
                c_cb_calls++;
                static uint32_t ans;
                ans = v * 3 + (t.target.ptr == CLIENT_PTR ? 1 : 0) + (t.cookie == CLIENT_COOKIE ? 1 : 0);
                struct binder_transaction_data rt;
                memset(&rt, 0, sizeof rt);
                rt.data_size = 4;
                rt.data.ptr.buffer = (uintptr_t)&ans;
                w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
                w32(&out, BC_REPLY); wbytes(&out, &rt, sizeof rt);
                break;
            }
            case BR_REPLY: {
                struct binder_transaction_data t;
                memcpy(&t, rb + p, sizeof t);
                p += sizeof t;
                size_t n = t.data_size;
                if (reply && reply_n) {
                    if (n > *reply_n) n = *reply_n;
                    memcpy(reply, (void *)(uintptr_t)t.data.ptr.buffer, n);
                    *reply_n = n;
                }
                w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
                result = (t.flags & TF_STATUS_CODE) ? 2 : 0;
                break;
            }
            case BR_DEAD_REPLY: result = BR_DEAD_REPLY; break;
            case BR_FAILED_REPLY: result = BR_FAILED_REPLY; break;
            case BR_DEAD_BINDER: case BR_CLEAR_DEATH_NOTIFICATION_DONE: p += 8; break;
            default:
                printf("  client: unexpected command 0x%x\n", cmd);
                return 3;
            }
        }
        if (out.n && wr(c_fd, out.b, out.n, NULL, 0, NULL) < 0) { perror("client write"); return 1; }
        if (result != 0xffffffff) return result;
    }
}

static int client(int ready_rd, bool bench, int bench_n)
{
    alarm(60);
    c_fd = open_binder("client");
    void *m = mmap(NULL, MAP_SZ, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, c_fd, 0);
    if (m == MAP_FAILED) { perror("client mmap"); return 2; }
    char c;
    if (read(ready_rd, &c, 1) != 1) { printf("client: no ready byte\n"); return 2; }

    if (bench) {
        uint32_t v = 7;
        uint8_t rep[16];
        size_t rn;
        for (int i = 0; i < 200; i++) { rn = sizeof rep; transact(0, C_PING, 0, &v, 4, NULL, 0, 0, rep, &rn); }
        double t0 = now_s();
        for (int i = 0; i < bench_n; i++) {
            rn = sizeof rep;
            if (transact(0, C_PING, 0, &v, 4, NULL, 0, 0, rep, &rn) != 0) { printf("bench: failed at %d\n", i); return 1; }
        }
        double t1 = now_s();
        printf("bench: %d sync round trips, %.1f us each\n", bench_n, (t1 - t0) / bench_n * 1e6);
        t0 = now_s();
        for (int i = 0; i < bench_n; i++)
            transact(0, C_ONEWAY, TF_ONE_WAY, &v, 4, NULL, 0, 0, NULL, NULL);
        uint32_t cnt = 0;
        do {
            size_t rn2 = sizeof rep;
            transact(0, C_COUNT, 0, NULL, 0, NULL, 0, 0, rep, &rn2);
            memcpy(&cnt, rep, 4);
        } while (cnt < (uint32_t)bench_n);
        t1 = now_s();
        printf("bench: %d oneway + 1 sync, %.1f us each\n", bench_n, (t1 - t0) / bench_n * 1e6);
        transact(0, C_QUIT, TF_ONE_WAY, NULL, 0, NULL, 0, 0, NULL, NULL);
        return 0;
    }

    // Sync call, reply, the sender's pid as the server saw it.
    uint32_t v = 41;
    uint8_t rep[256];
    size_t rn = sizeof rep;
    uint32_t r = transact(0, C_PING, 0, &v, 4, NULL, 0, 0, rep, &rn);
    uint32_t got = 0, spid = 0;
    if (rn >= 8) { memcpy(&got, rep, 4); memcpy(&spid, rep + 4, 4); }
    CHECK(r == 0 && got == 42 && spid == (uint32_t)getpid(),
          "sync transaction to handle 0: reply 42 to 41, server saw sender_pid %u (client %d)", spid, getpid());

    // A file descriptor, read on the other side.
    int pp[2];
    if (pipe(pp) != 0) return 2;
    const char *msg = "bytes through a binder fd";
    if (write(pp[1], msg, strlen(msg)) < 0) return 2;
    close(pp[1]);
    struct binder_fd_object fo;
    memset(&fo, 0, sizeof fo);
    fo.hdr.type = BINDER_TYPE_FD;
    fo.fd = (uint32_t)pp[0];
    uint64_t off0 = 0;
    rn = sizeof rep;
    r = transact(0, C_FD, 0, &fo, sizeof fo, &off0, 1, 0, rep, &rn);
    CHECK(r == 0 && rn == strlen(msg) && !memcmp(rep, msg, rn),
          "BINDER_TYPE_FD: the server read \"%.*s\" from its new descriptor", (int)rn, (char *)rep);
    close(pp[0]);

    // One of our objects: the server gets a handle, takes references and
    // asks for a death notification; we get BR_INCREFS/BR_ACQUIRE.
    struct flat_binder_object fb;
    memset(&fb, 0, sizeof fb);
    fb.hdr.type = BINDER_TYPE_BINDER;
    fb.flags = 0x7f | FLAT_BINDER_FLAG_ACCEPTS_FDS;
    fb.binder = CLIENT_PTR;
    fb.cookie = CLIENT_COOKIE;
    rn = sizeof rep;
    r = transact(0, C_REGISTER, 0, &fb, sizeof fb, &off0, 1, 0, rep, &rn);
    uint32_t h = 0;
    if (rn >= 4) memcpy(&h, rep, 4);
    CHECK(r == 0 && h >= 1, "BINDER_TYPE_BINDER sent: the server holds handle %u to it", h);

    // A call during our call: the server calls our object while we wait for
    // its reply; the driver routes it to this thread (transaction stack).
    v = 5;
    rn = sizeof rep;
    r = transact(0, C_CALLBACK, 0, &v, 4, NULL, 0, 0, rep, &rn);
    got = 0;
    if (rn >= 4) memcpy(&got, rep, 4);
    CHECK(r == 0 && c_cb_calls == 1 && got == 5 * 3 + 2,
          "nested call: server -> client object inside client -> server (answer %u, target ptr/cookie matched)", got);

    // Oneway: fire 20, then count them with sync calls. Oneway calls to one
    // node are delivered one at a time (the next only once the server frees
    // the previous buffer), so a sync call may overtake the queue, as on
    // Linux: ask until all have arrived.
    for (uint32_t i = 0; i < 20; i++)
        transact(0, C_ONEWAY, TF_ONE_WAY, &i, 4, NULL, 0, 0, NULL, NULL);
    uint32_t cnt = 0, order_ok = 0;
    int asks = 0;
    do {
        rn = sizeof rep;
        r = transact(0, C_COUNT, 0, NULL, 0, NULL, 0, 0, rep, &rn);
        if (rn >= 8) { memcpy(&cnt, rep, 4); memcpy(&order_ok, rep + 4, 4); }
        asks++;
    } while (r == 0 && cnt < 20 && asks < 200);
    CHECK(r == 0 && cnt == 20 && order_ok == 1,
          "20 oneway transactions arrived, in order (%u, seen after %d sync calls)", cnt, asks);

    // Scatter-gather: a PTR buffer holding a pointer to a second PTR buffer
    // (parent fixup), and an FDA of two descriptors inside the first.
    struct { uint64_t child_ptr; int32_t fds[2]; uint32_t pad; } parent_buf;
    char child_buf[32] = "the child buffer";
    int p1[2], p2[2];
    if (pipe(p1) || pipe(p2)) return 2;
    if (write(p1[1], "A", 1) < 0 || write(p2[1], "B", 1) < 0) return 2;
    parent_buf.child_ptr = (uintptr_t)child_buf;
    parent_buf.fds[0] = p1[0];
    parent_buf.fds[1] = p2[0];
    parent_buf.pad = 0;
    // Linux never fixes up into an object at data offset 0 ("nothing to fix
    // up in"), so a header word comes first, as parcels have.
    struct {
        uint64_t header;
        struct binder_buffer_object parent;
        struct binder_buffer_object child;
        struct binder_fd_array_object fda;
    } objs;
    memset(&objs, 0, sizeof objs);
    objs.header = 0x5347;
    objs.parent.hdr.type = BINDER_TYPE_PTR;
    objs.parent.buffer = (uintptr_t)&parent_buf;
    objs.parent.length = sizeof parent_buf;
    objs.child.hdr.type = BINDER_TYPE_PTR;
    objs.child.flags = BINDER_BUFFER_FLAG_HAS_PARENT;
    objs.child.buffer = (uintptr_t)child_buf;
    objs.child.length = sizeof child_buf;
    objs.child.parent = 0;
    objs.child.parent_offset = 0;
    objs.fda.hdr.type = BINDER_TYPE_FDA;
    objs.fda.num_fds = 2;
    objs.fda.parent = 0;
    objs.fda.parent_offset = 8;
    uint64_t soffs[3] = { 8, 8 + sizeof(struct binder_buffer_object), 8 + 2 * sizeof(struct binder_buffer_object) };
    rn = sizeof rep;
    r = transact(0, C_SG, 0, &objs, sizeof objs, soffs, 3, sizeof parent_buf + sizeof child_buf, rep, &rn);
    CHECK(r == 0 && rn >= 3 && !memcmp(rep, "ok", 2),
          "scatter-gather: parent fixup and FDA (server says: %.*s)", (int)rn, (char *)rep);

    // TF_CLEAR_BUF: the driver zeroes the buffer when the receiver frees it.
    char secret[32] = "a buffer to be cleared on free";
    rn = sizeof rep;
    r = transact(0, C_CLEAR, TF_CLEAR_BUF, secret, sizeof secret, NULL, 0, 0, rep, &rn);
    CHECK(r == 0 && rn >= 8 && rep[0] == 1 && rep[4] == 1,
          "TF_CLEAR_BUF: the server saw the data, and zeroes after BC_FREE_BUFFER");

    // Errors: an invalid handle.
    rn = sizeof rep;
    r = transact(77, C_PING, 0, &v, 4, NULL, 0, 0, rep, &rn);
    CHECK(r == BR_FAILED_REPLY, "transaction to invalid handle 77: BR_FAILED_REPLY");

    // Unmapping the receive buffer is binder_vma_close: nothing more can be
    // delivered here, so the server's reply to a new call fails and we get
    // BR_DEAD_REPLY (binder_alloc_new_buf: -ESRCH, "no vma").
    munmap(m, MAP_SZ);
    v = 1;
    rn = sizeof rep;
    r = transact(0, C_PING, 0, &v, 4, NULL, 0, 0, rep, &rn);
    CHECK(r == BR_DEAD_REPLY, "after munmap of the receive buffer, a reply cannot be delivered: BR_DEAD_REPLY");

    // Leave; the server should see our node die.
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------ server

static int server(int ready_wr, pid_t child, bool bench)
{
    alarm(60);
    int fd = open_binder("server");
    struct binder_version ver = { 0 };
    CHECK(ioctl(fd, BINDER_VERSION, &ver) == 0 && ver.protocol_version == BINDER_CURRENT_PROTOCOL_VERSION,
          "BINDER_VERSION %d", ver.protocol_version);
    void *bad = mmap(NULL, MAP_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(bad == MAP_FAILED && errno == EPERM, "mmap PROT_WRITE refused (EPERM), as Linux's FORBIDDEN_MMAP_FLAGS");
    void *m = mmap(NULL, MAP_SZ, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    CHECK(m != MAP_FAILED, "mmap %d bytes PROT_READ", MAP_SZ);
    void *m2 = mmap(NULL, MAP_SZ, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(m2 == MAP_FAILED && errno == EBUSY, "second mmap refused (EBUSY)");
    uint32_t zero = 0;
    CHECK(ioctl(fd, BINDER_SET_MAX_THREADS, &zero) == 0, "BINDER_SET_MAX_THREADS 0");
    struct flat_binder_object ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS;
    CHECK(ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, &ctx) == 0, "BINDER_SET_CONTEXT_MGR_EXT");
    int fd2 = open("/dev/binder", O_RDWR | O_CLOEXEC);
    CHECK(fd2 >= 0 && ioctl(fd2, BINDER_SET_CONTEXT_MGR, &zero) < 0 && errno == EBUSY,
          "a second context manager is refused (EBUSY)");
    if (fd2 >= 0) close(fd2);
    // The context manager calling handle 0 (itself) fails.
    {
        struct wb w = { .n = 0 };
        struct binder_transaction_data tr;
        memset(&tr, 0, sizeof tr);
        w32(&w, BC_TRANSACTION);
        wbytes(&w, &tr, sizeof tr);
        uint8_t rb[256];
        size_t got = 0;
        wr(fd, w.b, w.n, rb, sizeof rb, &got);
        bool failed = false;
        for (size_t p = 0; p + 4 <= got; p += 4) {
            uint32_t c;
            memcpy(&c, rb + p, 4);
            if (c == BR_FAILED_REPLY) failed = true;
        }
        CHECK(failed, "context manager transacting to handle 0: BR_FAILED_REPLY");
    }
    struct wb w = { .n = 0 };
    w32(&w, BC_ENTER_LOOPER);
    wr(fd, w.b, w.n, NULL, 0, NULL);
    // Poll the way servicemanager does (Looper + epoll on the binder fd);
    // non-blocking reads count any wakeup with nothing to read.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (write(ready_wr, "r", 1) != 1) return 2;

    uint32_t client_handle = 0, oneways = 0, next_oneway = 0, in_order = 1;
    int spurious = 0, wakeups = 0;
    bool dead = false, sg_ok = false;
    int fda_fds[2] = { -1, -1 };
    uint64_t fda_buf = 0;
    while (!dead) {
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 30000);
        if (pr <= 0) { printf("  MAL   server: poll timed out\n"); g_fail++; break; }
        wakeups++;
        uint8_t rb[512];
        size_t got = 0;
        int rr = wr(fd, NULL, 0, rb, sizeof rb, &got);
        if (rr < 0 && errno == EAGAIN) { spurious++; continue; }
        if (rr < 0) { perror("server read"); g_fail++; break; }
        bool useful = false;
        struct wb out = { .n = 0 };
        for (size_t q = 0; q + 4 <= got; ) {
            uint32_t cmd;
            memcpy(&cmd, rb + q, 4);
            q += 4;
            if (cmd == BR_NOOP) continue;
            useful = true;
            if (cmd == BR_INCREFS || cmd == BR_ACQUIRE || cmd == BR_RELEASE || cmd == BR_DECREFS) {
                struct binder_ptr_cookie pc;
                memcpy(&pc, rb + q, sizeof pc);
                q += sizeof pc;
                if (cmd == BR_INCREFS) { w32(&out, BC_INCREFS_DONE); wbytes(&out, &pc, sizeof pc); }
                if (cmd == BR_ACQUIRE) { w32(&out, BC_ACQUIRE_DONE); wbytes(&out, &pc, sizeof pc); }
                continue;
            }
            if (cmd == BR_TRANSACTION_COMPLETE || cmd == BR_SPAWN_LOOPER) continue;
            if (cmd == BR_DEAD_BINDER) {
                uint64_t ck;
                memcpy(&ck, rb + q, 8);
                q += 8;
                CHECK(ck == DEATH_COOKIE, "BR_DEAD_BINDER for the client's object after it exited (cookie 0x%llx)",
                      (unsigned long long)ck);
                w32(&out, BC_DEAD_BINDER_DONE); w64(&out, ck);
                w32(&out, BC_RELEASE); w32(&out, client_handle);
                w32(&out, BC_DECREFS); w32(&out, client_handle);
                dead = true;
                continue;
            }
            if (cmd == BR_CLEAR_DEATH_NOTIFICATION_DONE) { q += 8; continue; }
            if (cmd != BR_TRANSACTION && cmd != BR_TRANSACTION_SEC_CTX) {
                printf("  server: unexpected command 0x%x\n", cmd);
                g_fail++;
                break;
            }
            struct binder_transaction_data t;
            memcpy(&t, rb + q, sizeof t);
            q += cmd == BR_TRANSACTION_SEC_CTX ? sizeof(struct binder_transaction_data_secctx) : sizeof t;
            const uint8_t *data = (const uint8_t *)(uintptr_t)t.data.ptr.buffer;
            const uint64_t *offs = (const uint64_t *)(uintptr_t)t.data.ptr.offsets;
            static uint8_t reply[256];
            size_t rn = 0;
            bool oneway = t.flags & TF_ONE_WAY;
            bool free_now = true;
            switch (t.code) {
            case C_PING: {
                uint32_t v = 0;
                memcpy(&v, data, 4);
                v++;
                memcpy(reply, &v, 4);
                memcpy(reply + 4, &t.sender_pid, 4);
                rn = 8;
                break;
            }
            case C_FD: {
                struct binder_fd_object fo;
                memcpy(&fo, data + offs[0], sizeof fo);
                int nfd = (int)fo.fd;
                int cloexec = fcntl(nfd, F_GETFD);
                ssize_t n = read(nfd, reply, sizeof reply);
                rn = n > 0 ? (size_t)n : 0;
                if (!(cloexec & FD_CLOEXEC)) rn = 0;
                close(nfd);
                break;
            }
            case C_REGISTER: {
                struct flat_binder_object fb;
                memcpy(&fb, data + offs[0], sizeof fb);
                if (fb.hdr.type == BINDER_TYPE_HANDLE) {
                    client_handle = fb.handle;
                    w32(&out, BC_INCREFS); w32(&out, client_handle);
                    w32(&out, BC_ACQUIRE); w32(&out, client_handle);
                    w32(&out, BC_REQUEST_DEATH_NOTIFICATION);
                    struct binder_handle_cookie hc = { .handle = client_handle, .cookie = DEATH_COOKIE };
                    wbytes(&out, &hc, sizeof hc);
                }
                memcpy(reply, &client_handle, 4);
                rn = 4;
                break;
            }
            case C_CALLBACK: {
                // Call the client's object now, from inside its call.
                uint32_t v = 0;
                memcpy(&v, data, 4);
                w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
                free_now = false;
                struct binder_transaction_data ct;
                memset(&ct, 0, sizeof ct);
                ct.target.handle = client_handle;
                ct.code = C_CB;
                ct.data_size = 4;
                static uint32_t cv;
                cv = v;
                ct.data.ptr.buffer = (uintptr_t)&cv;
                w32(&out, BC_TRANSACTION); wbytes(&out, &ct, sizeof ct);
                wr(fd, out.b, out.n, NULL, 0, NULL);
                out.n = 0;
                // Wait for its reply (blocking, as a nested caller does).
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
                uint32_t ans = 0;
                for (bool done = false; !done; ) {
                    uint8_t b2[512];
                    size_t g2 = 0;
                    if (wr(fd, NULL, 0, b2, sizeof b2, &g2) < 0) break;
                    for (size_t z = 0; z + 4 <= g2; ) {
                        uint32_t c2;
                        memcpy(&c2, b2 + z, 4);
                        z += 4;
                        if (c2 == BR_REPLY) {
                            struct binder_transaction_data rt;
                            memcpy(&rt, b2 + z, sizeof rt);
                            z += sizeof rt;
                            memcpy(&ans, (void *)(uintptr_t)rt.data.ptr.buffer, 4);
                            struct wb f2 = { .n = 0 };
                            w32(&f2, BC_FREE_BUFFER); w64(&f2, rt.data.ptr.buffer);
                            wr(fd, f2.b, f2.n, NULL, 0, NULL);
                            done = true;
                        } else if (c2 == BR_FAILED_REPLY || c2 == BR_DEAD_REPLY) {
                            done = true;
                        } else if (c2 == BR_INCREFS || c2 == BR_ACQUIRE || c2 == BR_RELEASE || c2 == BR_DECREFS) {
                            z += 16;
                        }
                    }
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                memcpy(reply, &ans, 4);
                rn = 4;
                break;
            }
            case C_ONEWAY: {
                uint32_t v = 0;
                memcpy(&v, data, 4);
                if (!bench && v != next_oneway) in_order = 0;
                next_oneway = v + 1;
                oneways++;
                break;
            }
            case C_COUNT:
                memcpy(reply, &oneways, 4);
                memcpy(reply + 4, &in_order, 4);
                rn = 8;
                break;
            case C_SG: {
                // objects: parent PTR, child PTR (parent fixup at 0), FDA in parent at 8
                struct binder_buffer_object po, co;
                struct binder_fd_array_object fa;
                memcpy(&po, data + offs[0], sizeof po);
                memcpy(&co, data + offs[1], sizeof co);
                memcpy(&fa, data + offs[2], sizeof fa);
                const uint8_t *pb = (const uint8_t *)(uintptr_t)po.buffer;
                uint64_t child_ptr;
                memcpy(&child_ptr, pb, 8);
                int32_t f0, f1;
                memcpy(&f0, pb + 8, 4);
                memcpy(&f1, pb + 12, 4);
                char a = 0, b = 0;
                bool ptr_ok = child_ptr == co.buffer &&
                              !strcmp((const char *)(uintptr_t)co.buffer, "the child buffer");
                bool fd_ok = read(f0, &a, 1) == 1 && read(f1, &b, 1) == 1 && a == 'A' && b == 'B';
                sg_ok = ptr_ok && fd_ok && fa.num_fds == 2;
                snprintf((char *)reply, sizeof reply, "%s (pointer fixup %s, fds %s)",
                         sg_ok ? "ok" : "no", ptr_ok ? "right" : "WRONG", fd_ok ? "read" : "BAD");
                rn = strlen((char *)reply) + 1;
                fda_fds[0] = f0;
                fda_fds[1] = f1;
                fda_buf = t.data.ptr.buffer;
                break;
            }
            case C_CLEAR: {
                // Free now, then look at the (still mapped) buffer again.
                const char *d = (const char *)data;
                int32_t saw = t.data_size >= 8 && !memcmp(d, "a buffer", 8);
                struct wb f2 = { .n = 0 };
                w32(&f2, BC_FREE_BUFFER); w64(&f2, t.data.ptr.buffer);
                wr(fd, f2.b, f2.n, NULL, 0, NULL);
                free_now = false;
                int32_t zero = 1;
                for (uint64_t i = 0; i < t.data_size; i++) if (d[i]) zero = 0;
                memcpy(reply, &saw, 4);
                memcpy(reply + 4, &zero, 4);
                rn = 8;
                break;
            }
            case C_QUIT:
                dead = true;
                break;
            }
            if (free_now) {
                w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
            }
            if (!oneway) {
                struct binder_transaction_data rt;
                memset(&rt, 0, sizeof rt);
                rt.data_size = rn;
                rt.data.ptr.buffer = (uintptr_t)reply;
                w32(&out, BC_REPLY); wbytes(&out, &rt, sizeof rt);
            }
            if (t.code == C_SG) {
                // BC_FREE_BUFFER of an FDA buffer closes its descriptors.
                wr(fd, out.b, out.n, NULL, 0, NULL);
                out.n = 0;
                errno = 0;
                bool closed = fcntl(fda_fds[0], F_GETFD) < 0 && fcntl(fda_fds[1], F_GETFD) < 0;
                CHECK(closed, "FDA descriptors closed by BC_FREE_BUFFER (buffer 0x%llx)", (unsigned long long)fda_buf);
            }
        }
        if (out.n && wr(fd, out.b, out.n, NULL, 0, NULL) < 0) { perror("server write"); g_fail++; break; }
        if (!useful) spurious++;
    }
    if (!bench)
        CHECK(spurious == 0, "poll readiness: %d wakeups, %d with nothing to read", wakeups, spurious);
    else
        printf("  (bench server: %d wakeups, %d spurious)\n", wakeups, spurious);
    int st = 0;
    waitpid(child, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client exited cleanly (status 0x%x)", st);
    (void)sg_ok;
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------ pool
//
// `binder_ipc pool`: the thread pool libbinder runs. The context manager
// allows one extra looper (BINDER_SET_MAX_THREADS 1); the driver asks for it
// (BR_SPAWN_LOOPER), a pthread registers it (BC_REGISTER_LOOPER). A client
// with two threads then sends BLOCK, which the server holds in one looper
// until RELEASE arrives on the other: both server threads serve at once, and
// both client threads have calls in flight at once. The spawned looper
// leaves with BC_EXIT_LOOPER and BINDER_THREAD_EXIT.
enum { P_BLOCK = 20, P_RELEASE, P_DONE };
static int p_fd;
static pthread_mutex_t p_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t p_cv = PTHREAD_COND_INITIALIZER;
static bool p_released;
static int p_spawns, p_threads_seen;
static bool p_pool_exit_ok;

static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

// Serve until P_DONE. `registered`: this is the spawned thread.
static void *pool_loop(void *arg)
{
    bool spawned = arg != NULL;
    struct wb w = { .n = 0 };
    w32(&w, spawned ? BC_REGISTER_LOOPER : BC_ENTER_LOOPER);
    wr(p_fd, w.b, w.n, NULL, 0, NULL);
    pthread_mutex_lock(&p_mu);
    p_threads_seen++;
    pthread_mutex_unlock(&p_mu);
    bool mine_done = false;
    while (!mine_done) {
        uint8_t rb[512];
        size_t got = 0;
        if (wr(p_fd, NULL, 0, rb, sizeof rb, &got) < 0) break;
        struct wb out = { .n = 0 };
        for (size_t q = 0; q + 4 <= got; ) {
            uint32_t cmd;
            memcpy(&cmd, rb + q, 4);
            q += 4;
            if (cmd == BR_SPAWN_LOOPER) {
                pthread_mutex_lock(&p_mu);
                p_spawns++;
                pthread_mutex_unlock(&p_mu);
                pthread_t th;
                pthread_create(&th, NULL, pool_loop, (void *)1);
                pthread_detach(th);
                continue;
            }
            if (cmd == BR_INCREFS || cmd == BR_ACQUIRE || cmd == BR_RELEASE || cmd == BR_DECREFS) { q += 16; continue; }
            if (cmd == BR_DEAD_BINDER || cmd == BR_CLEAR_DEATH_NOTIFICATION_DONE) { q += 8; continue; }
            if (cmd != BR_TRANSACTION && cmd != BR_TRANSACTION_SEC_CTX) continue;
            struct binder_transaction_data t;
            memcpy(&t, rb + q, sizeof t);
            q += cmd == BR_TRANSACTION ? sizeof t : sizeof(struct binder_transaction_data_secctx);
            static _Thread_local int32_t ans[2];
            bool done_here = false;
            ans[0] = gettid_();
            ans[1] = 0;
            pthread_mutex_lock(&p_mu);
            if (t.code == P_BLOCK) {
                struct timespec dl;
                clock_gettime(CLOCK_REALTIME, &dl);
                dl.tv_sec += 10;
                while (!p_released)
                    if (pthread_cond_timedwait(&p_cv, &p_mu, &dl) != 0) break;
                ans[1] = p_released;
            } else if (t.code == P_RELEASE) {
                p_released = true;
                pthread_cond_broadcast(&p_cv);
                ans[1] = 1;
            } else if (t.code == P_DONE) {
                done_here = true;
            }
            pthread_mutex_unlock(&p_mu);
            w32(&out, BC_FREE_BUFFER); w64(&out, t.data.ptr.buffer);
            if (!(t.flags & TF_ONE_WAY)) {
                struct binder_transaction_data rt;
                memset(&rt, 0, sizeof rt);
                rt.data_size = sizeof ans;
                rt.data.ptr.buffer = (uintptr_t)ans;
                w32(&out, BC_REPLY); wbytes(&out, &rt, sizeof rt);
            }
            wr(p_fd, out.b, out.n, NULL, 0, NULL);
            out.n = 0;
            if (done_here) { mine_done = true; break; }
        }
        if (out.n) wr(p_fd, out.b, out.n, NULL, 0, NULL);
    }
    if (spawned) {
        struct wb x = { .n = 0 };
        w32(&x, BC_EXIT_LOOPER);
        wr(p_fd, x.b, x.n, NULL, 0, NULL);
        int zero = 0;
        pthread_mutex_lock(&p_mu);
        p_pool_exit_ok = ioctl(p_fd, BINDER_THREAD_EXIT, &zero) == 0;
        pthread_mutex_unlock(&p_mu);
    }
    return NULL;
}

struct pcall { uint32_t code; int32_t ans[2]; uint32_t r; int delay_ms; };
static volatile int g_sigs;
static void on_usr1(int s) { (void)s; g_sigs++; }
static void *pool_client_call(void *arg)
{
    struct pcall *c = arg;
    if (c->delay_ms) usleep((useconds_t)c->delay_ms * 1000);
    size_t rn = sizeof c->ans;
    c->r = transact(0, c->code, 0, NULL, 0, NULL, 0, 0, (uint8_t *)c->ans, &rn);
    return NULL;
}

static int g_pool_bench;
static int pool_test(void)
{
    printf("== binder pool (pid %d)\n", getpid());
    int pp[2];
    if (pipe(pp) != 0) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        alarm(30);
        close(pp[1]);
        c_fd = open_binder("pool client");
        if (mmap(NULL, MAP_SZ, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, c_fd, 0) == MAP_FAILED) _exit(2);
        char c;
        if (read(pp[0], &c, 1) != 1) _exit(2);
        // SIGUSR1 without SA_RESTART reaches the BLOCK caller while it waits
        // in BINDER_WRITE_READ: -EINTR, retried with the consumed counts.
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        struct pcall a = { .code = P_BLOCK }, b = { .code = P_RELEASE, .delay_ms = 400 };
        pthread_t ta, tb;
        pthread_create(&ta, NULL, pool_client_call, &a);
        pthread_create(&tb, NULL, pool_client_call, &b);
        usleep(150000);
        pthread_kill(ta, SIGUSR1);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        CHECK(g_sigs == 1 && g_eintr >= 1 && a.r == 0,
              "a signal during a blocked call: handler ran, EINTR x%d, the call then completed", g_eintr);
        CHECK(a.r == 0 && b.r == 0 && a.ans[1] == 1 && b.ans[1] == 1,
              "two client threads, two calls in flight: BLOCK (held) released by RELEASE");
        CHECK(a.ans[0] != b.ans[0] && a.ans[0] && b.ans[0],
              "served by two different server threads (tids %d and %d)", a.ans[0], b.ans[0]);
        if (g_pool_bench > 0) {
            // Round trips to a looper parked in BINDER_WRITE_READ (the
            // libbinder thread-pool path: no doorbell, no poll).
            struct pcall x = { .code = P_RELEASE };
            for (int i = 0; i < 200; i++) pool_client_call(&x);
            double t0 = now_s();
            for (int i = 0; i < g_pool_bench; i++) pool_client_call(&x);
            double t1 = now_s();
            printf("bench: %d sync round trips to a parked looper, %.1f us each\n",
                   g_pool_bench, (t1 - t0) / g_pool_bench * 1e6);
        }
        // One DONE per server looper: each leaves after answering one.
        struct pcall d = { .code = P_DONE }, e = { .code = P_DONE };
        pool_client_call(&d);
        pool_client_call(&e);
        _exit(g_fail ? 1 : 0);
    }
    close(pp[0]);
    alarm(30);
    p_fd = open_binder("pool server");
    if (mmap(NULL, MAP_SZ, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, p_fd, 0) == MAP_FAILED) return 2;
    uint32_t one = 1;
    ioctl(p_fd, BINDER_SET_MAX_THREADS, &one);
    struct flat_binder_object ctx;
    memset(&ctx, 0, sizeof ctx);
    if (ioctl(p_fd, BINDER_SET_CONTEXT_MGR_EXT, &ctx) != 0) { perror("SET_CONTEXT_MGR_EXT"); return 1; }
    if (write(pp[1], "r", 1) != 1) return 2;
    pool_loop(NULL);
    int st = 0;
    waitpid(pid, &st, 0);
    usleep(200000);         // the spawned looper's exit
    pthread_mutex_lock(&p_mu);
    CHECK(p_spawns == 1 && p_threads_seen == 2, "BR_SPAWN_LOOPER once (max threads 1): %d spawned, %d loopers",
          p_spawns, p_threads_seen);
    CHECK(p_pool_exit_ok, "the spawned looper left with BC_EXIT_LOOPER + BINDER_THREAD_EXIT");
    pthread_mutex_unlock(&p_mu);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "pool client exited cleanly (status 0x%x)", st);
    if (!g_fail) printf("== binder pool: ok\n");
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "pool")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        g_pool_bench = argc > 2 ? atoi(argv[2]) : 0;
        return pool_test();
    }
    bool bench = argc > 1 && !strcmp(argv[1], "bench");
    int n = bench && argc > 2 ? atoi(argv[2]) : 2000;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("== binder ipc%s (pid %d)\n", bench ? " bench" : "", getpid());
    int pp[2];
    if (pipe(pp) != 0) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        close(pp[1]);
        int r = client(pp[0], bench, n);
        fflush(stdout);
        _exit(r);
    }
    close(pp[0]);
    int r = server(pp[1], pid, bench);
    if (r == 0 && !bench) printf("== binder ipc: ok\n");
    return r;
}
