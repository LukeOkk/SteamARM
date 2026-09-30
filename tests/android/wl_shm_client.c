// A Wayland wl_shm client with no libraries at all: raw system calls and the
// Wayland wire protocol by hand, so the same source builds for x86-64 (run
// under FEX, in any x86-64 root) and for aarch64 (native under lxrun).
// docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Display";
// benchmarks/stage27-android-display.txt.
//
// What it proves when it runs against a compositor in another process: the
// pixels it writes into a memfd it mapped MAP_SHARED reach the compositor,
// which maps the same memfd after receiving it over the socket
// (SCM_RIGHTS). That is exactly how Waydroid's hwcomposer hands Android's
// frames to a compositor when gralloc is not gbm (wl_shm).
//
//   wl_shm_client [-g] [-w W] [-h H] [-n FRAMES] [-t TITLE] [-H HOLD_MS]
//     -g            print the compositor's globals (name, interface, version)
//                   and exit, like wayland-info
//     -n FRAMES     frames to draw, each after the previous frame callback
//     -H HOLD_MS    keep the window (and the last frame) that long at the end
//   Socket: $WAYLAND_DISPLAY (absolute path, or relative to
//   $XDG_RUNTIME_DIR; default "wayland-0").
//
// Each frame is four quadrants in fixed colours (0xff2080c0 top left,
// 0xffc04020 top right, 0xff20c040 bottom left, 0xffe0e0e0 bottom right)
// with a black band that moves one row per frame, so a screenshot of the
// compositor's output can be checked by colour. Last line:
//   "== wl_shm_client: <frames> frames in <ms> ms, <fps> fps (<arch>)"
// or "== wl_shm_client: FAIL <reason>".
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;

#if defined(__x86_64__)
#define ARCH "x86_64"
enum { NR_read = 0, NR_write = 1, NR_close = 3, NR_mmap = 9, NR_munmap = 11, NR_socket = 41,
       NR_connect = 42, NR_sendmsg = 46, NR_recvmsg = 47, NR_ftruncate = 77,
       NR_clock_gettime = 228, NR_exit_group = 231, NR_memfd_create = 319, NR_ppoll = 271 };
static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    i64 r;
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    register i64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
// The entry point: the stack holds argc, argv[], NULL, envp[]; C wants a
// 16-byte aligned stack less the return address.
__asm__(".globl _start\n_start:\n xor %rbp,%rbp\n mov %rsp,%rdi\n and $-16,%rsp\n call cstart\n hlt\n");
#elif defined(__aarch64__)
#define ARCH "aarch64"
enum { NR_read = 63, NR_write = 64, NR_close = 57, NR_mmap = 222, NR_munmap = 215, NR_socket = 198,
       NR_connect = 203, NR_sendmsg = 211, NR_recvmsg = 212, NR_ftruncate = 46,
       NR_clock_gettime = 113, NR_exit_group = 94, NR_memfd_create = 279, NR_ppoll = 73 };
static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    register i64 x4 __asm__("x4") = e;
    register i64 x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
__asm__(".globl _start\n_start:\n mov x29, #0\n mov x30, #0\n mov x0, sp\n and sp, x0, #-16\n bl cstart\n brk #0\n");
#else
#error "x86_64 or aarch64"
#endif
#define sys3(n, a, b, c) sys6(n, a, b, c, 0, 0, 0)

// ------------------------------------------------------------ tiny libc
static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sys3(NR_write, 1, (i64)s, (i64)slen(s)); }
static void outn(i64 v)
{
    char b[24];
    int i = 23;
    int neg = v < 0;
    u64 u = (u64)v;
    if (neg) u = ~u + 1;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    out(b + i);
}
static void outhex(u64 v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) {
        int d = (int)((v >> (28 - 4 * i)) & 15);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[10] = 0;
    out(b);
}
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static i64 atoi_(const char *s) { i64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static void mcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static __attribute__((noreturn)) void die(const char *why, i64 err)
{
    out("== wl_shm_client: FAIL "); out(why);
    if (err) { out(" ("); outn(err); out(", "); outhex((u64)err); out(")"); }
    out(" ("ARCH")\n");
    sys3(NR_exit_group, 1, 0, 0);
    for (;;) {}
}
static u64 now_ns(void)
{
    i64 ts[2];
    sys3(NR_clock_gettime, 1 /* CLOCK_MONOTONIC */, (i64)ts, 0);
    return (u64)ts[0] * 1000000000ull + (u64)ts[1];
}
// Clang may emit calls to these for struct copies and zeroing.
void *memset(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { mcpy(d, s, n); return d; }

// ------------------------------------------------------------ the wire
static int g_fd;
static u32 g_out[1024];            // one message being built
static u32 g_on;
static u8 g_in[65536];             // bytes received, not yet parsed
static u64 g_inn;

static void msg_begin(u32 obj, u32 op) { g_out[0] = obj; g_out[1] = op; g_on = 2; }
static void put_u(u32 v) { g_out[g_on++] = v; }
static void put_s(const char *s)
{
    u32 n = (u32)slen(s) + 1;
    put_u(n);
    u8 *p = (u8 *)&g_out[g_on];
    mcpy(p, s, n);
    u32 words = (n + 3) / 4;
    for (u32 i = n; i < words * 4; i++) p[i] = 0;
    g_on += words;
}
static void msg_send(int pass_fd)
{
    g_out[1] |= (g_on * 4) << 16;
    struct { u64 base, len; } iov = { (u64)g_out, g_on * 4 };
    struct { u64 len; i32 level, type; i32 fd; i32 pad; } cm = { 20, 1 /* SOL_SOCKET */, 1 /* SCM_RIGHTS */, pass_fd, 0 };
    struct { u64 name; u32 namelen, pad0; u64 iov, iovlen, ctl, ctllen; i32 flags, pad1; } mh = {
        0, 0, 0, (u64)&iov, 1, pass_fd >= 0 ? (u64)&cm : 0, pass_fd >= 0 ? 24 : 0, 0, 0 };
    i64 r = sys3(NR_sendmsg, g_fd, (i64)&mh, 0x4000 /* MSG_NOSIGNAL */);
    if (r != (i64)(g_on * 4)) die("sendmsg", r);
}

// Events of interest land here; the dispatcher calls it per message.
struct global { u32 name, version; char iface[64]; };
static struct global g_globals[128];
static int g_nglobals;
// Client object ids must be handed out densely (libwayland-server's
// wl_map_insert_at refuses an id past the next free one): a counter, plus
// the ids the compositor has released (wl_display.delete_id) for reuse.
static u32 g_next_id = 3;
static u32 g_free_ids[64], g_nfree;
static u32 new_id(void) { return g_nfree ? g_free_ids[--g_nfree] : g_next_id++; }
static u32 g_sync_id, g_sync_done;
static u32 g_frame_id, g_frame_done;
static u32 g_configure_serial, g_configured;
static u32 g_wm_base_id, g_xdg_surface_id, g_toplevel_id;
static u32 g_buffer_ids[2], g_busy[2];
static u32 g_toplevel_w, g_toplevel_h, g_closed;
static u32 g_shm_formats, g_shm_id;

static void handle(u32 obj, u32 op, u32 *a, u32 words)
{
    if (obj == 1 && op == 0) {                       // wl_display.error
        out("wl_display.error: object "); outn(a[0]); out(" code "); outn(a[1]);
        out(" "); if (words > 3) out((const char *)&a[3]); out("\n");
        die("protocol error", 0);
    } else if (obj == 1 && op == 1) {                // wl_display.delete_id
        if (g_nfree < 64) g_free_ids[g_nfree++] = a[0];
    } else if (obj == 2 && op == 0) {                // wl_registry.global
        if (g_nglobals < 128) {
            struct global *g = &g_globals[g_nglobals++];
            g->name = a[0];
            u32 n = a[1];
            if (n > 63) n = 63;
            mcpy(g->iface, &a[2], n);
            g->iface[n] = 0;
            g->version = a[2 + (a[1] + 3) / 4];
        }
    } else if (obj == g_sync_id && g_sync_id && op == 0) {
        g_sync_done = 1;
        g_sync_id = 0;                               // the id is released next
    } else if (obj == g_frame_id && g_frame_id && op == 0) {
        g_frame_done = 1;
        g_frame_id = 0;
    } else if (obj == g_wm_base_id && g_wm_base_id && op == 0) {   // ping
        msg_begin(g_wm_base_id, 3); put_u(a[0]); msg_send(-1);     // pong
    } else if (obj == g_xdg_surface_id && g_xdg_surface_id && op == 0) {
        g_configure_serial = a[0];
        g_configured = 1;
    } else if (obj == g_toplevel_id && g_toplevel_id && op == 0) {
        g_toplevel_w = a[0]; g_toplevel_h = a[1];
    } else if (obj == g_toplevel_id && g_toplevel_id && op == 1) {
        g_closed = 1;
    } else if (obj == g_buffer_ids[0] && op == 0) {
        g_busy[0] = 0;
    } else if (obj == g_buffer_ids[1] && op == 0) {
        g_busy[1] = 0;
    } else if (obj == g_shm_id && op == 0) {         // wl_shm.format
        if (a[0] == 1) g_shm_formats |= 1;           // XRGB8888
        if (a[0] == 0) g_shm_formats |= 2;           // ARGB8888
    }
}

// Read what is there (blocking once), dispatch every complete message.
static void dispatch(void)
{
    i64 r = sys3(NR_read, g_fd, (i64)(g_in + g_inn), (i64)(sizeof g_in - g_inn));
    if (r <= 0) die("compositor closed the connection", r);
    g_inn += (u64)r;
    u64 at = 0;
    while (g_inn - at >= 8) {
        u32 *h = (u32 *)(g_in + at);
        u32 size = h[1] >> 16, op = h[1] & 0xffff;
        if (size < 8 || size > sizeof g_in) die("bad message size", size);
        if (g_inn - at < size) break;
        handle(h[0], op, h + 2, (size - 8) / 4);
        at += size;
    }
    for (u64 i = at; i < g_inn; i++) g_in[i - at] = g_in[i];
    g_inn -= at;
}
static void roundtrip(void)
{
    g_sync_id = new_id();
    g_sync_done = 0;
    msg_begin(1, 0); put_u(g_sync_id); msg_send(-1);    // wl_display.sync
    while (!g_sync_done) dispatch();
}
static u32 find_global(const char *iface, u32 *version)
{
    for (int i = 0; i < g_nglobals; i++)
        if (seq(g_globals[i].iface, iface)) { if (version) *version = g_globals[i].version; return g_globals[i].name; }
    return 0;
}
static u32 bind(const char *iface, u32 want_version, u32 id)
{
    u32 version = 0, name = find_global(iface, &version);
    if (!name) { out("no global "); out(iface); out("\n"); die("missing global", 0); }
    if (version > want_version) version = want_version;
    msg_begin(2, 0); put_u(name); put_s(iface); put_u(version); put_u(id); msg_send(-1);
    return version;
}

// ------------------------------------------------------------ drawing
static const u32 Q[4] = { 0xff2080c0u, 0xffc04020u, 0xff20c040u, 0xffe0e0e0u };
static void draw(u32 *px, u32 w, u32 h, u32 frame)
{
    u32 band = frame % h;
    for (u32 y = 0; y < h; y++) {
        u32 *row = px + (u64)y * w;
        if (y >= band && y < band + 4) {
            for (u32 x = 0; x < w; x++) row[x] = 0xff000000u;
            continue;
        }
        u32 top = y < h / 2 ? 0 : 2;
        for (u32 x = 0; x < w; x++) row[x] = Q[top + (x < w / 2 ? 0 : 1)];
    }
}

void cstart(u64 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    u32 W = 480, H = 320, FRAMES = 300, HOLD = 0, globals_only = 0;
    const char *title = "wl_shm_client " ARCH;
    for (int i = 1; i < argc; i++) {
        if (seq(argv[i], "-g")) globals_only = 1;
        else if (seq(argv[i], "-w") && i + 1 < argc) W = (u32)atoi_(argv[++i]);
        else if (seq(argv[i], "-h") && i + 1 < argc) H = (u32)atoi_(argv[++i]);
        else if (seq(argv[i], "-n") && i + 1 < argc) FRAMES = (u32)atoi_(argv[++i]);
        else if (seq(argv[i], "-H") && i + 1 < argc) HOLD = (u32)atoi_(argv[++i]);
        else if (seq(argv[i], "-t") && i + 1 < argc) title = argv[++i];
    }
    const char *xdg = 0, *disp = "wayland-0";
    for (char **e = envp; *e; e++) {
        const char *s = *e;
        if (s[0] == 'X' && s[1] == 'D' && s[2] == 'G' && s[3] == '_' && s[4] == 'R' && s[5] == 'U' &&
            s[6] == 'N' && s[7] == 'T' && s[8] == 'I' && s[9] == 'M' && s[10] == 'E' && s[11] == '_' &&
            s[12] == 'D' && s[13] == 'I' && s[14] == 'R' && s[15] == '=') xdg = s + 16;
        if (s[0] == 'W' && s[1] == 'A' && s[2] == 'Y' && s[3] == 'L' && s[4] == 'A' && s[5] == 'N' &&
            s[6] == 'D' && s[7] == '_' && s[8] == 'D' && s[9] == 'I' && s[10] == 'S' && s[11] == 'P' &&
            s[12] == 'L' && s[13] == 'A' && s[14] == 'Y' && s[15] == '=') disp = s + 16;
    }
    // sockaddr_un: family, then the path (at most 107 bytes and a NUL).
    struct { unsigned short fam; char path[108]; } sa;
    sa.fam = 1;
    u64 n = 0;
    if (disp[0] != '/') {
        if (!xdg) die("XDG_RUNTIME_DIR not set", 0);
        for (u64 i = 0; xdg[i] && n < 100; i++) sa.path[n++] = xdg[i];
        sa.path[n++] = '/';
    }
    for (u64 i = 0; disp[i] && n < 106; i++) sa.path[n++] = disp[i];
    sa.path[n] = 0;
    g_fd = (int)sys3(NR_socket, 1 /* AF_UNIX */, 1 | 0x80000 /* SOCK_STREAM|CLOEXEC */, 0);
    if (g_fd < 0) die("socket", g_fd);
    i64 r = sys3(NR_connect, g_fd, (i64)&sa, (i64)(2 + n + 1));
    if (r < 0) { out("socket path "); out(sa.path); out("\n"); die("connect", r); }

    msg_begin(1, 1); put_u(2); msg_send(-1);             // wl_display.get_registry -> 2
    roundtrip();
    if (globals_only) {
        for (int i = 0; i < g_nglobals; i++) {
            outn(g_globals[i].name); out(" "); out(g_globals[i].iface); out(" v");
            outn(g_globals[i].version); out("\n");
        }
        out("== wl_shm_client: "); outn(g_nglobals); out(" globals ("ARCH")\n");
        sys3(NR_exit_group, 0, 0, 0);
    }
    u32 comp = new_id(), shm = new_id(), wm = new_id();
    u32 cv = bind("wl_compositor", 4, comp);
    bind("wl_shm", 1, shm);
    bind("xdg_wm_base", 1, wm);
    g_shm_id = shm;
    g_wm_base_id = wm;
    roundtrip();                                        // wl_shm.format events
    if (!(g_shm_formats & 1)) die("no XRGB8888 in wl_shm", 0);

    // The buffer: a memfd, grown, mapped shared, and sent by SCM_RIGHTS.
    u64 stride = (u64)W * 4, one = stride * H, size = one * 2;
    int mfd = (int)sys3(NR_memfd_create, (i64)"wl_shm_client", 1 | 2 /* CLOEXEC|ALLOW_SEALING */, 0);
    if (mfd < 0) die("memfd_create", mfd);
    r = sys3(NR_ftruncate, mfd, (i64)size, 0);
    if (r < 0) die("ftruncate", r);
    u32 *px = (u32 *)sys6(NR_mmap, 0, (i64)size, 3 /* RW */, 1 /* MAP_SHARED */, mfd, 0);
    if ((i64)px < 0 && (i64)px > -4096) die("mmap", (i64)px);
    u32 pool = new_id();
    msg_begin(shm, 0); put_u(pool); put_u((u32)size); msg_send(mfd);   // wl_shm.create_pool
    for (u32 b = 0; b < 2; b++) {
        g_buffer_ids[b] = new_id();
        msg_begin(pool, 0); put_u(g_buffer_ids[b]); put_u((u32)(one * b)); put_u(W); put_u(H); put_u((u32)stride);
        put_u(1 /* XRGB8888 */); msg_send(-1);          // wl_shm_pool.create_buffer
    }
    u32 surf = new_id(), xsurf = new_id(), top = new_id();
    msg_begin(comp, 0); put_u(surf); msg_send(-1);       // wl_compositor.create_surface
    g_xdg_surface_id = xsurf;
    msg_begin(wm, 2); put_u(xsurf); put_u(surf); msg_send(-1);   // xdg_wm_base.get_xdg_surface
    g_toplevel_id = top;
    msg_begin(xsurf, 1); put_u(top); msg_send(-1);       // xdg_surface.get_toplevel
    msg_begin(top, 2); put_s(title); msg_send(-1);       // xdg_toplevel.set_title
    msg_begin(top, 3); put_s("steamarm.wl_shm_client"); msg_send(-1);   // set_app_id
    msg_begin(surf, 6); msg_send(-1);                    // wl_surface.commit (no buffer)
    while (!g_configured) dispatch();
    msg_begin(xsurf, 4); put_u(g_configure_serial); msg_send(-1);   // ack_configure
    out("connected: "); out(sa.path); out(", wl_compositor v"); outn(cv); out(", configure ");
    outn(g_toplevel_w); out("x"); outn(g_toplevel_h); out(", buffer "); outn(W); out("x"); outn(H);
    out(", pool "); outn((i64)size); out(" bytes at "); outhex((u64)px >> 32); out(":"); outhex((u64)px & 0xffffffffu);
    out("\n");

    u64 t0 = 0, draw_ns = 0;
    u32 done = 0;
    for (u32 f = 0; f < FRAMES && !g_closed; f++) {
        u32 b = f & 1;
        while (g_busy[b]) dispatch();                   // wait for wl_buffer.release
        u64 d0 = now_ns();
        draw(px + (u64)b * W * H, W, H, f);
        draw_ns += now_ns() - d0;
        g_frame_id = new_id();
        g_frame_done = 0;
        msg_begin(surf, 1); put_u(g_buffer_ids[b]); put_u(0); put_u(0); msg_send(-1);   // attach
        if (cv >= 4) { msg_begin(surf, 9); } else { msg_begin(surf, 2); }
        put_u(0); put_u(0); put_u(W); put_u(H); msg_send(-1);                            // damage
        msg_begin(surf, 3); put_u(g_frame_id); msg_send(-1);                             // frame
        msg_begin(surf, 6); msg_send(-1);                                                // commit
        g_busy[b] = 1;
        while (!g_frame_done && !g_closed) dispatch();
        if (f == 0) t0 = now_ns();                      // first frame shown: start the clock
        done++;
    }
    u64 ms = (now_ns() - t0) / 1000000ull;
    if (HOLD) {
        u64 until = now_ns() + (u64)HOLD * 1000000ull;
        while (now_ns() < until && !g_closed) {
            struct { i64 s, ns; } ts = { 0, 50000000 };
            struct { i32 fd; short ev, rev; } p = { g_fd, 1, 0 };
            if (sys6(NR_ppoll, (i64)&p, 1, (i64)&ts, 0, 8, 0) > 0) dispatch();
        }
    }
    out("== wl_shm_client: "); outn(done); out(" frames in "); outn((i64)ms); out(" ms, ");
    u64 fps10 = ms ? (u64)(done - 1) * 10000ull / ms : 0;
    outn((i64)(fps10 / 10)); out("."); outn((i64)(fps10 % 10)); out(" fps, draw ");
    outn((i64)(done ? draw_ns / done / 1000 : 0)); out(" us/frame ("ARCH")\n");
    sys3(NR_exit_group, 0, 0, 0);
    for (;;) {}
}
