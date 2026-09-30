// Android's system property service as a host process: `lxrun
// --property-service <dir> <root> [--daemon]` (runtime/props.h).
//
// What init's property service does at boot and afterwards, ported from the
// sources of the image's own Android 11 (LineageOS 18.1, VERIFIED IN SOURCE
// against LineageOS/android_system_core lineage-18.1 9242067 and
// LineageOS/android_bionic lineage-18.1 b67b033):
//
//   property_info    the serialized trie of property_contexts that maps a
//                    name to its context (area file) and type:
//                    property_service/libpropertyinfoserializer
//                    (trie_builder.cpp, trie_serializer.cpp,
//                    property_info_file.cpp), read back as
//                    libpropertyinfoparser/property_info_parser.cpp does;
//   the areas        one 128 KiB file per context plus properties_serial,
//                    bionic's prop_area / prop_bt / prop_info layout
//                    (libc/system_properties/prop_area.cpp, prop_info.cpp),
//                    written with SystemProperties::Update/Add's serial
//                    protocol (system_properties.cpp): dirty bit, backup
//                    area, release fences, a futex wake on the property's
//                    serial and on the global one;
//   boot values      init/property_service.cpp PropertyInit: the "kernel"
//                    boot properties (LXRT_PROPERTY_BOOTARGS stands for the
//                    command line), PropertyLoadBootDefaults' files in its
//                    order with its import/filter/override rules, the derived
//                    ro.product.* and ro.build.fingerprint, then
//                    ro.property_service.version and the persistent ones;
//   setprop          handle_property_set_fd: PROP_MSG_SETPROP and
//                    PROP_MSG_SETPROP2 on <dir>/property_service, with
//                    IsLegalPropertyName/Value, the type check, ro.* written
//                    once, persist.* stored (init/persistent_properties.cpp's
//                    protobuf file, by default the guest's own
//                    /data/property/persistent_properties).
//
// Not done, and said so: no SELinux (the MAC checks of CheckPermissions and
// CanReadProperty are not applied; every area is readable), no init to
// receive ctl.* (refused with PROP_ERROR_HANDLE_CONTROL_MESSAGE and logged),
// no property triggers (nothing runs "on property:..."), no
// vendor_load_properties hook, no restorecon (selinux.restorecon_recursive is
// set at once). Every guest is the Mac user, so "ro.* is write-once" is the
// protocol's rule, not a security boundary.
//
// Guests map the areas read-only and MAP_SHARED; this process writes through
// its own MAP_SHARED mapping of the same files, so both see the same pages
// (the host page cache), and __ulock_wake with the process-shared flavour
// reaches the guests' FUTEX_WAIT on those words (runtime/futex_ops.c parks
// shared futexes in shared mappings on that flavour). MEASURED before this
// file was written: two unrelated processes mapping one file, the waiter
// read-only, woke on UL_COMPARE_AND_WAIT_SHARED (benchmarks/stage26-android-
// properties.txt).
//
// The state is the area files, not this process: a service that left
// (LXRT_PROPERTY_IDLE) and is started again adopts them as they are. The
// property state of an Android root is reset ("reboot") by removing the
// directory.
#include "props.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_NO_ERRNO               0x01000000u
#define ULF_WAKE_ALL               0x00000100u

// bionic libc/system_properties (prop_area.cpp, prop_info.h) and
// libc/include/sys/_system_properties.h
#define PA_SIZE            (128 * 1024)
#define PROP_AREA_MAGIC    0x504f5250u
#define PROP_AREA_VERSION  0xfc6ed0abu
#define PA_HDR             128          // sizeof(prop_area): 4 + 4 + 4 + 4 + 28 * 4
#define PA_DATA_SIZE       (PA_SIZE - PA_HDR)
#define PROP_BT_HDR        20           // namelen, prop, left, right, children
#define PROP_INFO_HDR      96           // serial + value[PROP_VALUE_MAX]
#define PROP_VALUE_MAX     92
#define PROP_NAME_MAX      32
#define LONG_FLAG          (1u << 16)
#define LONG_OFFSET_AT     (4 + 56)     // long_property.offset inside prop_info
static const char kLongLegacyError[] = "Must use __system_property_read_callback() to read";

#define PROP_MSG_SETPROP                  1
#define PROP_MSG_SETPROP2                 0x00020001u
#define PROP_SUCCESS                      0
#define PROP_ERROR_READ_CMD               0x0004
#define PROP_ERROR_READ_DATA              0x0008
#define PROP_ERROR_READ_ONLY_PROPERTY     0x000B
#define PROP_ERROR_INVALID_NAME           0x0010
#define PROP_ERROR_INVALID_VALUE          0x0014
#define PROP_ERROR_PERMISSION_DENIED      0x0018
#define PROP_ERROR_INVALID_CMD            0x001B
#define PROP_ERROR_HANDLE_CONTROL_MESSAGE 0x0020
#define PROP_ERROR_SET_FAILED             0x0024

static const char *g_dir;              // the service's directory
static struct sockaddr_un g_init_sa;   // LXRT_PROPERTY_INIT: an init to tell (see init_notify)
static int g_init_fd = -1;
static char g_root[PATH_MAX];          // the Android root (host path)
static char g_persist[PATH_MAX];       // persistent property file (host path)
static bool g_persist_loaded;
static int g_vendor_api = 30;
static volatile sig_atomic_t g_stop;

// ------------------------------------------------------------------ logging

static void plog(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld [%d] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000, (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { plog("out of memory"); exit(70); }
    return p;
}
static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { plog("out of memory"); exit(70); }
    return p;
}
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xmalloc(n), s, n); }
static bool starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

// ------------------------------------------------------------ root paths
//
// The image's symlinks are guest paths (/system_ext -> /system/system_ext,
// /odm/etc -> /vendor/odm/etc). A guest path is resolved here the way a
// kernel resolves one inside a chroot: component by component, ".", ".."
// and links (absolute ones from the root, relative ones from their
// directory) handled here, never above the root. The host path that comes
// out has no link left in it except, possibly, a last component that does
// not exist yet.
static bool root_path(const char *guest, char *out, size_t outn)
{
    char rest[PATH_MAX];            // what is still to walk
    char done[PATH_MAX] = "";       // the resolved guest path ("" is "/")
    if (guest[0] != '/' || snprintf(rest, sizeof rest, "%s", guest) >= (int)sizeof rest)
        return false;
    int links = 0;
    const char *p = rest;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t cl = strcspn(p, "/");
        char comp[NAME_MAX + 1];
        if (cl > NAME_MAX) return false;
        memcpy(comp, p, cl);
        comp[cl] = '\0';
        p += cl;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *s = strrchr(done, '/');
            if (s) *s = '\0';
            continue;
        }
        char cand[PATH_MAX], host[PATH_MAX], tgt[PATH_MAX];
        if (snprintf(cand, sizeof cand, "%s/%s", done, comp) >= (int)sizeof cand ||
            snprintf(host, sizeof host, "%s%s", g_root, cand) >= (int)sizeof host)
            return false;
        ssize_t tl = readlink(host, tgt, sizeof tgt - 1);
        if (tl > 0) {
            if (++links > 40) return false;
            tgt[tl] = '\0';
            char next[PATH_MAX];
            if (snprintf(next, sizeof next, "%s/%s", tgt, p) >= (int)sizeof next) return false;
            memcpy(rest, next, strlen(next) + 1);
            p = rest;
            if (tgt[0] == '/') done[0] = '\0';
            continue;
        }
        memcpy(done, cand, strlen(cand) + 1);
    }
    return snprintf(out, outn, "%s%s", g_root, done[0] ? done : "/") < (int)outn;
}

static char *read_guest_file(const char *guest, size_t *len)
{
    char host[PATH_MAX];
    if (!root_path(guest, host, sizeof host))
        return NULL;
    int fd = open(host, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (64 << 20)) {
        close(fd);
        return NULL;
    }
    char *d = xmalloc((size_t)st.st_size + 2);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, d + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    d[got] = '\0';
    if (len) *len = got;
    return d;
}

static bool guest_readable(const char *guest)
{
    char host[PATH_MAX];
    return root_path(guest, host, sizeof host) && access(host, R_OK) == 0;
}

// ------------------------------------------------ property_contexts -> trie
//
// libpropertyinfoserializer, in C. Strings are kept as pointers into two
// sorted sets (contexts, types) whose indexes the serializer writes.

struct pentry { char *name; const char *ctx; const char *type; };
struct tnode {
    char *name;
    const char *ctx, *type;             // the node's own match ("a.b." lines)
    struct tnode **kids; int nkids;
    struct pentry *pre; int npre;       // prefix matches ending at this node
    struct pentry *ex; int nex;         // exact matches ending at this node
};
struct strset { char **s; int n; };

static const char *strset_add(struct strset *set, const char *s)
{
    for (int i = 0; i < set->n; i++)
        if (!strcmp(set->s[i], s)) return set->s[i];
    set->s = xrealloc(set->s, sizeof(char *) * (size_t)(set->n + 1));
    return set->s[set->n++] = xstrdup(s);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

struct builder { struct tnode root; struct strset ctxs, types; };

static struct tnode *tnode_child(struct tnode *n, const char *name, bool add)
{
    for (int i = 0; i < n->nkids; i++)
        if (!strcmp(n->kids[i]->name, name)) return n->kids[i];
    if (!add) return NULL;
    struct tnode *c = calloc(1, sizeof *c);
    if (!c) { plog("out of memory"); exit(70); }
    c->name = xstrdup(name);
    n->kids = xrealloc(n->kids, sizeof *n->kids * (size_t)(n->nkids + 1));
    n->kids[n->nkids++] = c;
    return c;
}

// TrieBuilder::AddToTrie. false (and *err) on a duplicate, as init.
static bool builder_add(struct builder *b, const char *name, const char *ctx, const char *type,
                        bool exact, char *err, size_t errn)
{
    const char *c = strset_add(&b->ctxs, ctx);
    const char *t = strset_add(&b->types, type);
    // android::base::Split(name, "."), a trailing "" dropped (ends_with_dot).
    char *copy = xstrdup(name);
    char *pieces[256];
    int np = 0;
    for (char *p = copy, *s = copy; ; p++) {
        if (*p == '.' || *p == '\0') {
            bool end = *p == '\0';
            *p = '\0';
            if (np < 256) pieces[np++] = s;
            s = p + 1;
            if (end) break;
        }
    }
    bool ends_with_dot = false;
    if (np > 0 && pieces[np - 1][0] == '\0') { ends_with_dot = true; np--; }
    bool ok = true;
    if (np == 0) { free(copy); snprintf(err, errn, "Empty property entry '%s'", name); return false; }
    struct tnode *cur = &b->root;
    for (int i = 0; i < np - 1; i++)
        cur = tnode_child(cur, pieces[i], true);
    const char *last = pieces[np - 1];
    if (exact) {
        for (int i = 0; i < cur->nex; i++)
            if (!strcmp(cur->ex[i].name, last)) ok = false;
        if (!ok) snprintf(err, errn, "Duplicate exact match detected for '%s'", name);
        else {
            cur->ex = xrealloc(cur->ex, sizeof *cur->ex * (size_t)(cur->nex + 1));
            cur->ex[cur->nex++] = (struct pentry){ xstrdup(last), c, t };
        }
    } else if (!ends_with_dot) {
        for (int i = 0; i < cur->npre; i++)
            if (!strcmp(cur->pre[i].name, last)) ok = false;
        if (!ok) snprintf(err, errn, "Duplicate prefix match detected for '%s'", name);
        else {
            cur->pre = xrealloc(cur->pre, sizeof *cur->pre * (size_t)(cur->npre + 1));
            cur->pre[cur->npre++] = (struct pentry){ xstrdup(last), c, t };
        }
    } else {
        struct tnode *child = tnode_child(cur, last, true);
        if (child->ctx || child->type) {
            ok = false;
            snprintf(err, errn, "Duplicate prefix match detected for '%s'", name);
        } else {
            child->ctx = c;
            child->type = t;
        }
    }
    free(copy);
    return ok;
}

// ParsePropertyInfoFile: one "name context [prefix|exact] [type...]" per line.
static void parse_contexts(struct builder *b, const char *guest, bool require_prefix_or_exact,
                           bool *build_ok)
{
    char *data = read_guest_file(guest, NULL);
    if (!data) {
        plog("Could not read properties from '%s'", guest);
        return;
    }
    char *save = NULL;
    for (char *line = data; line; line = save) {
        char *nl = strchr(line, '\n');
        save = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        while (isspace((unsigned char)*line)) line++;
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!*line || *line == '#')
            continue;
        char *tok[64];
        int nt = 0;
        for (char *p = line; *p && nt < 64; ) {
            tok[nt++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) { *p++ = '\0'; while (isspace((unsigned char)*p)) p++; }
        }
        if (nt < 2) {
            plog("Could not read line from '%s': no context entry in '%s'", guest, line);
            continue;
        }
        const char *match = nt > 2 ? tok[2] : "";
        bool exact = false;
        if (!strcmp(match, "exact")) exact = true;
        else if (strcmp(match, "prefix") && strcmp(match, "") && require_prefix_or_exact) {
            plog("Could not read line from '%s': Match operation '%s' is not valid", guest, match);
            continue;
        }
        // Join(type_strings, " ").
        char type[512] = "";
        size_t tl = 0;
        for (int i = 3; i < nt && tl < sizeof type; i++)
            tl += (size_t)snprintf(type + tl, sizeof type - tl, "%s%s", i > 3 ? " " : "", tok[i]);
        if (tl >= sizeof type) {
            plog("Could not read line from '%s': type too long", guest);
            continue;
        }
        if (nt > 3) {
            // IsTypeValid
            bool valid;
            if (!strcmp(tok[3], "enum")) valid = nt > 4;
            else {
                valid = nt == 4;
                static const char *const simple[] = { "string", "bool", "int", "uint", "double", "size" };
                bool known = false;
                for (size_t k = 0; k < sizeof simple / sizeof simple[0]; k++)
                    if (!strcmp(tok[3], simple[k])) known = true;
                valid = valid && known;
            }
            if (!valid) {
                plog("Could not read line from '%s': Type '%s' is not valid", guest, type);
                continue;
            }
        }
        char err[256];
        if (!builder_add(b, tok[0], tok[1], type, exact, err, sizeof err)) {
            plog("Unable to serialize property contexts: %s", err);
            *build_ok = false;
        }
    }
    free(data);
}

// The arena (trie_node_arena.h): every allocation rounded up to 4 bytes.
struct arena { uint8_t *d; uint32_t len, cap; };
static uint32_t arena_alloc(struct arena *a, size_t size)
{
    uint32_t al = (uint32_t)((size + 3) & ~(size_t)3);
    if (a->len + al > a->cap) {
        uint32_t nc = (a->len + al + a->cap) * 2;
        a->d = xrealloc(a->d, nc);
        memset(a->d + a->cap, 0, nc - a->cap);
        a->cap = nc;
    }
    uint32_t off = a->len;
    a->len += al;
    return off;
}
static uint32_t *arena_u32(struct arena *a, uint32_t off) { return (uint32_t *)(void *)(a->d + off); }
static uint32_t arena_string(struct arena *a, const char *s)
{
    uint32_t off = arena_alloc(a, strlen(s) + 1);
    strcpy((char *)a->d + off, s);
    return off;
}

struct serializer { struct arena a; char **ctxs; int nctx; char **types; int ntypes; };

static uint32_t index_of(char **set, int n, const char *s)
{
    if (!s || !*s) return ~0u;
    char *const *hit = bsearch(&s, set, (size_t)n, sizeof *set, cmp_str);
    return hit ? (uint32_t)(hit - set) : ~0u;
}

static void serialize_strings(struct arena *a, char **s, int n)
{
    uint32_t cnt = arena_alloc(a, 4);
    *arena_u32(a, cnt) = (uint32_t)n;
    uint32_t arr = arena_alloc(a, 4 * (size_t)n);
    for (int i = 0; i < n; i++) {
        uint32_t so = arena_string(a, s[i]);
        arena_u32(a, arr)[i] = so;
    }
}

static uint32_t write_entry(struct serializer *z, const char *name, const char *ctx, const char *type)
{
    uint32_t ci = index_of(z->ctxs, z->nctx, ctx);
    uint32_t ti = index_of(z->types, z->ntypes, type);
    uint32_t off = arena_alloc(&z->a, 16);
    uint32_t no = arena_string(&z->a, name);
    uint32_t *e = arena_u32(&z->a, off);
    e[0] = no;
    e[1] = (uint32_t)strlen(name);
    e[2] = ci;
    e[3] = ti;
    return off;
}

// The serializer sorts with std::sort; ties cannot change what a lookup
// finds (two prefixes of equal length cannot both match one name), so a
// stable insertion sort gives the same trie up to the order of equal-length
// prefixes.
static int cmp_pre_len_desc(const struct pentry *x, const struct pentry *y)
{
    size_t a = strlen(x->name), b = strlen(y->name);
    return a > b ? -1 : a < b ? 1 : 0;
}
static int cmp_pentry_name(const struct pentry *x, const struct pentry *y) { return strcmp(x->name, y->name); }
static void sort_entries(struct pentry *v, int n, int (*cmp)(const struct pentry *, const struct pentry *))
{
    for (int i = 1; i < n; i++) {
        struct pentry k = v[i];
        int j = i - 1;
        while (j >= 0 && cmp(&k, &v[j]) < 0) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}
static int cmp_tnode(const void *a, const void *b)
{
    return strcmp((*(struct tnode *const *)a)->name, (*(struct tnode *const *)b)->name);
}

static uint32_t write_node(struct serializer *z, struct tnode *n)
{
    uint32_t t = arena_alloc(&z->a, 28);
    uint32_t pe = write_entry(z, n->name, n->ctx, n->type);
    arena_u32(&z->a, t)[0] = pe;

    sort_entries(n->pre, n->npre, cmp_pre_len_desc);
    arena_u32(&z->a, t)[3] = (uint32_t)n->npre;
    uint32_t pa = arena_alloc(&z->a, 4 * (size_t)n->npre);
    arena_u32(&z->a, t)[4] = pa;
    for (int i = 0; i < n->npre; i++) {
        uint32_t o = write_entry(z, n->pre[i].name, n->pre[i].ctx, n->pre[i].type);
        arena_u32(&z->a, pa)[i] = o;
    }

    sort_entries(n->ex, n->nex, cmp_pentry_name);
    arena_u32(&z->a, t)[5] = (uint32_t)n->nex;
    uint32_t ea = arena_alloc(&z->a, 4 * (size_t)n->nex);
    arena_u32(&z->a, t)[6] = ea;
    for (int i = 0; i < n->nex; i++) {
        uint32_t o = write_entry(z, n->ex[i].name, n->ex[i].ctx, n->ex[i].type);
        arena_u32(&z->a, ea)[i] = o;
    }

    qsort(n->kids, (size_t)n->nkids, sizeof *n->kids, cmp_tnode);
    arena_u32(&z->a, t)[1] = (uint32_t)n->nkids;
    uint32_t ca = arena_alloc(&z->a, 4 * (size_t)n->nkids);
    arena_u32(&z->a, t)[2] = ca;
    for (int i = 0; i < n->nkids; i++) {
        uint32_t o = write_node(z, n->kids[i]);
        arena_u32(&z->a, ca)[i] = o;
    }
    return t;
}

// TrieSerializer::SerializeTrie. Returns the bytes of property_info.
static uint8_t *serialize_trie(struct builder *b, uint32_t *size)
{
    struct serializer z = { 0 };
    z.ctxs = b->ctxs.s; z.nctx = b->ctxs.n;
    z.types = b->types.s; z.ntypes = b->types.n;
    qsort(z.ctxs, (size_t)z.nctx, sizeof *z.ctxs, cmp_str);
    qsort(z.types, (size_t)z.ntypes, sizeof *z.types, cmp_str);
    uint32_t h = arena_alloc(&z.a, 24);
    arena_u32(&z.a, h)[0] = 1;                    // current_version
    arena_u32(&z.a, h)[1] = 1;                    // minimum_supported_version
    arena_u32(&z.a, h)[3] = z.a.len;              // contexts_offset
    serialize_strings(&z.a, z.ctxs, z.nctx);
    arena_u32(&z.a, h)[4] = z.a.len;              // types_offset
    serialize_strings(&z.a, z.types, z.ntypes);
    arena_u32(&z.a, h)[2] = z.a.len;              // size, for the index lookups
    uint32_t root = write_node(&z, &b->root);
    arena_u32(&z.a, h)[5] = root;
    arena_u32(&z.a, h)[2] = z.a.len;
    *size = z.a.len;
    return z.a.d;
}

// -------------------------------------------------- property_info lookups
//
// property_info_parser.cpp GetPropertyInfoIndexes, bounds-checked.
static uint8_t *g_pi;       // property_info bytes (a private copy)
static uint32_t g_pi_size;

static uint32_t pi_u32(uint32_t off)
{
    if (off > g_pi_size || g_pi_size - off < 4) return ~0u;
    uint32_t v;
    memcpy(&v, g_pi + off, 4);
    return v;
}
static const char *pi_str(uint32_t off)
{
    if (off >= g_pi_size) return "";
    if (!memchr(g_pi + off, 0, g_pi_size - off)) return "";
    return (const char *)g_pi + off;
}
static uint32_t pi_num_contexts(void) { return pi_u32(pi_u32(12)); }
static const char *pi_context(uint32_t i) { return pi_str(pi_u32(pi_u32(12) + 4 + 4 * i)); }
static const char *pi_type(uint32_t i) { return pi_str(pi_u32(pi_u32(16) + 4 + 4 * i)); }

// A trie node's fields: 0 property_entry, 1 num_child_nodes, 2 child_nodes,
// 3 num_prefixes, 4 prefix_entries, 5 num_exact_matches, 6 exact_match_entries.
static uint32_t node_f(uint32_t node, int f) { return pi_u32(node + 4 * (uint32_t)f); }
static uint32_t entry_f(uint32_t entry, int f) { return pi_u32(entry + 4 * (uint32_t)f); }

static void check_prefix(const char *rem, uint32_t node, uint32_t *ci, uint32_t *ti)
{
    size_t rl = strlen(rem);
    uint32_t n = node_f(node, 3), arr = node_f(node, 4);
    for (uint32_t i = 0; i < n && i < 100000; i++) {
        uint32_t e = pi_u32(arr + 4 * i);
        uint32_t pl = entry_f(e, 1);
        if (pl > rl) continue;
        if (!strncmp(pi_str(entry_f(e, 0)), rem, pl)) {
            if (entry_f(e, 2) != ~0u) *ci = entry_f(e, 2);
            if (entry_f(e, 3) != ~0u) *ti = entry_f(e, 3);
            return;
        }
    }
}

static bool find_child(uint32_t node, const char *name, uint32_t len, uint32_t *out)
{
    uint32_t n = node_f(node, 1), arr = node_f(node, 2);
    int lo = 0, hi = (int)n - 1;
    while (hi >= lo) {
        int mid = (lo + hi) / 2;
        uint32_t child = pi_u32(arr + 4 * (uint32_t)mid);
        const char *cn = pi_str(entry_f(node_f(child, 0), 0));
        int c = strncmp(cn, name, len);
        if (c == 0 && cn[len] != '\0') c = 1;
        if (c == 0) { *out = child; return true; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

static void pi_lookup(const char *name, uint32_t *context_index, uint32_t *type_index)
{
    uint32_t rci = ~0u, rti = ~0u;
    const char *rem = name;
    uint32_t node = pi_u32(20);                 // root_offset
    for (;;) {
        const char *sep = strchr(rem, '.');
        uint32_t e = node_f(node, 0);
        if (entry_f(e, 2) != ~0u) rci = entry_f(e, 2);
        if (entry_f(e, 3) != ~0u) rti = entry_f(e, 3);
        check_prefix(rem, node, &rci, &rti);
        if (!sep) break;
        uint32_t child;
        if (!find_child(node, rem, (uint32_t)(sep - rem), &child)) break;
        node = child;
        rem = sep + 1;
    }
    uint32_t n = node_f(node, 5), arr = node_f(node, 6);
    for (uint32_t i = 0; i < n && i < 100000; i++) {
        uint32_t e = pi_u32(arr + 4 * i);
        if (!strcmp(pi_str(entry_f(e, 0)), rem)) {
            *context_index = entry_f(e, 2) != ~0u ? entry_f(e, 2) : rci;
            *type_index = entry_f(e, 3) != ~0u ? entry_f(e, 3) : rti;
            return;
        }
    }
    check_prefix(rem, node, &rci, &rti);
    *context_index = rci;
    *type_index = rti;
}

// ------------------------------------------------------------- the areas

struct area { char *ctx; uint8_t *base; };
static struct area *g_areas;          // one per context, property_info's order
static uint32_t g_nareas;
static uint8_t *g_serial_area;        // properties_serial

static uint32_t *pa_bytes_used(uint8_t *b) { return (uint32_t *)(void *)b; }
static _Atomic uint32_t *pa_serial(uint8_t *b) { return (_Atomic uint32_t *)(void *)(b + 4); }
static uint8_t *pa_data(uint8_t *b) { return b + PA_HDR; }
static _Atomic uint32_t *au32(uint8_t *p) { return (_Atomic uint32_t *)(void *)p; }

static void futex_wake_all(void *addr)
{
    // The guests' FUTEX_WAIT on a MAP_SHARED mapping parks on this flavour
    // (runtime/futex_ops.c); -ENOENT is "nobody waiting".
    __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO, addr, 0);
}

static void *pa_obj(uint8_t *b, uint32_t off)
{
    if (off > PA_DATA_SIZE) return NULL;
    return pa_data(b) + off;
}

static void *pa_allocate(uint8_t *b, size_t size, uint32_t *off)
{
    uint32_t al = (uint32_t)((size + 3) & ~(size_t)3);
    uint32_t used = *pa_bytes_used(b);
    if ((uint64_t)used + al > PA_DATA_SIZE)
        return NULL;
    *off = used;
    *pa_bytes_used(b) = used + al;
    return pa_data(b) + used;
}

static uint8_t *new_prop_bt(uint8_t *b, const char *name, uint32_t len, uint32_t *off)
{
    uint8_t *p = pa_allocate(b, PROP_BT_HDR + len + 1, off);
    if (!p) return NULL;
    memcpy(p, &len, 4);
    memcpy(p + PROP_BT_HDR, name, len);
    p[PROP_BT_HDR + len] = '\0';
    return p;
}

static uint8_t *new_prop_info(uint8_t *b, const char *name, uint32_t namelen, const char *value,
                              uint32_t valuelen, uint32_t *off)
{
    uint32_t noff;
    uint8_t *p = pa_allocate(b, PROP_INFO_HDR + namelen + 1, &noff);
    if (!p) return NULL;
    memcpy(p + PROP_INFO_HDR, name, namelen);
    p[PROP_INFO_HDR + namelen] = '\0';
    if (valuelen >= PROP_VALUE_MAX) {
        uint32_t loff;
        char *lv = pa_allocate(b, valuelen + 1, &loff);
        if (!lv) return NULL;
        memcpy(lv, value, valuelen);
        lv[valuelen] = '\0';
        loff -= noff;                   // relative to the prop_info
        atomic_store_explicit(au32(p), (uint32_t)(strlen(kLongLegacyError) << 24) | LONG_FLAG,
                              memory_order_relaxed);
        memcpy(p + 4, kLongLegacyError, sizeof kLongLegacyError);
        memcpy(p + LONG_OFFSET_AT, &loff, 4);
    } else {
        atomic_store_explicit(au32(p), valuelen << 24, memory_order_relaxed);
        memcpy(p + 4, value, valuelen);
        p[4 + valuelen] = '\0';
    }
    *off = noff;
    return p;
}

static int cmp_prop_name(const char *one, uint32_t one_len, const char *two, uint32_t two_len)
{
    if (one_len < two_len) return -1;
    if (one_len > two_len) return 1;
    return strncmp(one, two, one_len);
}

// prop_area::find_prop_bt: siblings are a binary tree ordered by length,
// then bytes.
static uint8_t *find_prop_bt(uint8_t *b, uint8_t *bt, const char *name, uint32_t len, bool alloc)
{
    uint8_t *cur = bt;
    for (;;) {
        if (!cur) return NULL;
        uint32_t cl;
        memcpy(&cl, cur, 4);
        int r = cmp_prop_name(name, len, (const char *)cur + PROP_BT_HDR, cl);
        if (r == 0) return cur;
        _Atomic uint32_t *link = au32(cur + (r < 0 ? 8 : 12));
        uint32_t o = atomic_load_explicit(link, memory_order_relaxed);
        if (o) { cur = pa_obj(b, o); continue; }
        if (!alloc) return NULL;
        uint32_t no;
        uint8_t *nb = new_prop_bt(b, name, len, &no);
        if (nb) atomic_store_explicit(link, no, memory_order_release);
        return nb;
    }
}

// prop_area::find_property: the trie of '.'-separated pieces.
static uint8_t *find_property(uint8_t *b, const char *name, uint32_t namelen, const char *value,
                              uint32_t valuelen, bool alloc)
{
    const char *rem = name;
    uint8_t *cur = pa_obj(b, 0);        // the root node
    for (;;) {
        const char *sep = strchr(rem, '.');
        bool sub = sep != NULL;
        uint32_t sl = sub ? (uint32_t)(sep - rem) : (uint32_t)strlen(rem);
        if (!sl) return NULL;
        uint8_t *root = NULL;
        uint32_t co = atomic_load_explicit(au32(cur + 16), memory_order_relaxed);
        if (co) root = pa_obj(b, co);
        else if (alloc) {
            uint32_t no;
            root = new_prop_bt(b, rem, sl, &no);
            if (root) atomic_store_explicit(au32(cur + 16), no, memory_order_release);
        }
        if (!root) return NULL;
        cur = find_prop_bt(b, root, rem, sl, alloc);
        if (!cur) return NULL;
        if (!sub) break;
        rem = sep + 1;
    }
    uint32_t po = atomic_load_explicit(au32(cur + 4), memory_order_relaxed);
    if (po) return pa_obj(b, po);
    if (!alloc) return NULL;
    uint32_t no;
    uint8_t *pi = new_prop_info(b, name, namelen, value, valuelen, &no);
    if (pi) atomic_store_explicit(au32(cur + 4), no, memory_order_release);
    return pi;
}

static void foreach_bt(uint8_t *b, uint8_t *bt, void (*fn)(const char *, const char *, void *), void *ck)
{
    if (!bt) return;
    uint32_t o;
    if ((o = atomic_load_explicit(au32(bt + 8), memory_order_relaxed))) foreach_bt(b, pa_obj(b, o), fn, ck);
    if ((o = atomic_load_explicit(au32(bt + 4), memory_order_relaxed))) {
        uint8_t *pi = pa_obj(b, o);
        if (pi) {
            uint32_t s = atomic_load_explicit(au32(pi), memory_order_relaxed);
            const char *v = (s & LONG_FLAG) ? (const char *)pi + *(uint32_t *)(void *)(pi + LONG_OFFSET_AT)
                                            : (const char *)pi + 4;
            fn((const char *)pi + PROP_INFO_HDR, v, ck);
        }
    }
    if ((o = atomic_load_explicit(au32(bt + 16), memory_order_relaxed))) foreach_bt(b, pa_obj(b, o), fn, ck);
    if ((o = atomic_load_explicit(au32(bt + 12), memory_order_relaxed))) foreach_bt(b, pa_obj(b, o), fn, ck);
}

static void prop_foreach(void (*fn)(const char *, const char *, void *), void *ck)
{
    for (uint32_t i = 0; i < g_nareas; i++)
        if (g_areas[i].base) foreach_bt(g_areas[i].base, pa_obj(g_areas[i].base, 0), fn, ck);
}

static uint8_t *area_for(const char *name)
{
    uint32_t ci, ti;
    pi_lookup(name, &ci, &ti);
    if (ci == ~0u || ci >= g_nareas || !g_areas[ci].base) {
        plog("Could not find context for property \"%s\"", name);
        return NULL;
    }
    return g_areas[ci].base;
}

static uint8_t *prop_find(const char *name)
{
    uint8_t *b = area_for(name);
    return b ? find_property(b, name, (uint32_t)strlen(name), NULL, 0, false) : NULL;
}

static void bump_global_serial(void)
{
    _Atomic uint32_t *s = pa_serial(g_serial_area);
    atomic_store_explicit(s, atomic_load_explicit(s, memory_order_relaxed) + 1, memory_order_release);
    futex_wake_all((void *)s);
}

// SystemProperties::Update, step for step.
static int prop_update(uint8_t *pi, const char *value, uint32_t len)
{
    if (len >= PROP_VALUE_MAX) return -1;
    uint8_t *b = area_for((const char *)pi + PROP_INFO_HDR);
    if (!b) return -1;
    uint32_t serial = atomic_load_explicit(au32(pi), memory_order_relaxed);
    uint32_t old_len = serial >> 24;
    memcpy(pa_data(b) + PROP_BT_HDR, pi + 4, old_len + 1);     // the dirty backup area
    atomic_thread_fence(memory_order_release);
    serial |= 1;
    atomic_store_explicit(au32(pi), serial, memory_order_relaxed);
    memcpy(pi + 4, value, len);
    pi[4 + len] = '\0';
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(au32(pi), (len << 24) | ((serial + 1) & 0xffffff), memory_order_relaxed);
    futex_wake_all(pi);
    bump_global_serial();
    return 0;
}

// SystemProperties::Add.
static int prop_add(const char *name, const char *value, uint32_t valuelen)
{
    uint32_t namelen = (uint32_t)strlen(name);
    if (valuelen >= PROP_VALUE_MAX && !starts_with(name, "ro.")) return -1;
    if (namelen < 1) return -1;
    uint8_t *b = area_for(name);
    if (!b) return -1;
    if (!find_property(b, name, namelen, value, valuelen, true)) return -1;
    bump_global_serial();
    return 0;
}

static const char *prop_value(uint8_t *pi)
{
    uint32_t s = atomic_load_explicit(au32(pi), memory_order_acquire);
    if (s & LONG_FLAG) {
        uint32_t lo;
        memcpy(&lo, pi + LONG_OFFSET_AT, 4);
        return (const char *)pi + lo;
    }
    return (const char *)pi + 4;
}

static const char *get_prop(const char *name, const char *def)
{
    uint8_t *pi = prop_find(name);
    return pi ? prop_value(pi) : def;
}

// ------------------------------------------------------ PropertySet & co

// init/util.cpp IsLegalPropertyName.
static bool legal_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || name[0] == '.' || name[n - 1] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '.') { if (name[i - 1] == '.') return false; continue; }
        if (c == '_' || c == '-' || c == '@' || c == ':') continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        return false;
    }
    return true;
}

// mbstowcs(nullptr, value, 0) != -1 in bionic's UTF-8 C locale: well-formed
// UTF-8, no overlong forms, no surrogates, nothing past U+10FFFF.
static bool valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n; ) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        int k;
        uint32_t cp, min;
        if ((c & 0xe0) == 0xc0) { k = 1; cp = c & 0x1f; min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { k = 2; cp = c & 0x0f; min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        for (int j = 1; j <= k; j++) {
            if (i + (size_t)j >= n || (s[i + j] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i + j] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += (size_t)k + 1;
    }
    return true;
}

// init/property_type.cpp CheckType.
static bool check_type(const char *type, const char *value)
{
    if (!*value) return true;
    if (!type || !*type) return false;
    char first[64];
    size_t fl = strcspn(type, " ");
    if (fl >= sizeof first) return false;
    memcpy(first, type, fl);
    first[fl] = '\0';
    if (!strcmp(first, "string")) return true;
    if (!strcmp(first, "bool"))
        return !strcmp(value, "true") || !strcmp(value, "false") || !strcmp(value, "1") || !strcmp(value, "0");
    if (!strcmp(first, "int") || !strcmp(first, "uint")) {
        // android::base::ParseInt/ParseUint (libbase parseint.h): leading
        // spaces skipped, base 16 after "0x", else 10, the whole string,
        // no '-' for uint.
        bool is_uint = !strcmp(first, "uint");
        if (is_uint && value[0] == '-') return false;
        const char *s = value;
        while (isspace((unsigned char)*s)) s++;
        if (is_uint && *s == '-') return false;
        int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
        char *end;
        errno = 0;
        if (is_uint) (void)strtoull(s, &end, base);
        else (void)strtoll(s, &end, base);
        return errno == 0 && end != s && *end == '\0';
    }
    if (!strcmp(first, "double")) {
        // ParseDouble: strtod, the whole string, within [lowest, max].
        char *end;
        errno = 0;
        double d = strtod(value, &end);
        return errno == 0 && end != value && *end == '\0' && d >= -1.7976931348623157e308 &&
               d <= 1.7976931348623157e308;
    }
    if (!strcmp(first, "size")) {
        const char *p = value;
        while (*p && isdigit((unsigned char)*p)) p++;
        if (p == value || !*p || (*p != 'g' && *p != 'k' && *p != 'm')) return false;
        return p[1] == '\0';
    }
    if (!strcmp(first, "enum")) {
        const char *p = type + fl;
        while (*p) {
            while (*p == ' ') p++;
            size_t l = strcspn(p, " ");
            if (l && strlen(value) == l && !strncmp(p, value, l)) return true;
            p += l;
        }
    }
    return false;
}

static void write_persistent(const char *name, const char *value);

// An init to hand control messages to and to tell about every property that
// changes: LXRT_PROPERTY_INIT names a Unix datagram socket of this user
// (scripts/android-boot.py binds one). Android's init is both the property
// service and the one that acts on ctl.* and "on property:" triggers; here
// they are separate processes, and this is the line between them. One
// datagram per event, sent without waiting: "ctl\0<cmd>\0<value>\0<pid>" or
// "set\0<name>\0<value>\0<pid>". An init that is not reading loses them.
static bool init_notify(const char *kind, const char *a, const char *b, int pid)
{
    if (g_init_fd < 0) return false;
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "%s%c%s%c%s%c%d", kind, 0, a, 0, b, 0, pid);
    if (n < 0) return false;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    return sendto(g_init_fd, buf, (size_t)n, MSG_DONTWAIT, (struct sockaddr *)&g_init_sa,
                  sizeof g_init_sa) == n;
}

static void init_channel_open(void)
{
    const char *p = getenv("LXRT_PROPERTY_INIT");
    if (!p || !*p) return;
    struct stat st;
    // The socket must be this user's own: nothing else is told about
    // properties or asked to start services.
    if (lstat(p, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
        plog("LXRT_PROPERTY_INIT=%s: not a socket of this user; ctl.* stays refused", p);
        return;
    }
    g_init_sa.sun_family = AF_UNIX;
    if (snprintf(g_init_sa.sun_path, sizeof g_init_sa.sun_path, "%s", p) >= (int)sizeof g_init_sa.sun_path) {
        plog("LXRT_PROPERTY_INIT=%s: path too long", p);
        return;
    }
    g_init_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_init_fd >= 0) {
        fcntl(g_init_fd, F_SETFD, FD_CLOEXEC);
        plog("control messages and property changes go to the init at %s", p);
    }
}

// init's PropertySet.
static uint32_t property_set(const char *name, const char *value, const char **error)
{
    size_t vl = strlen(value);
    if (!legal_name(name)) { *error = "Illegal property name"; return PROP_ERROR_INVALID_NAME; }
    if (vl >= PROP_VALUE_MAX && !starts_with(name, "ro.")) { *error = "Property value too long"; return PROP_ERROR_INVALID_VALUE; }
    if (!valid_utf8((const unsigned char *)value, vl)) { *error = "Value is not a UTF8 encoded string"; return PROP_ERROR_INVALID_VALUE; }
    uint8_t *pi = prop_find(name);
    if (pi) {
        if (starts_with(name, "ro.")) { *error = "Read-only property was already set"; return PROP_ERROR_READ_ONLY_PROPERTY; }
        prop_update(pi, value, (uint32_t)vl);
    } else if (prop_add(name, value, (uint32_t)vl) < 0) {
        *error = "__system_property_add failed";
        return PROP_ERROR_SET_FAILED;
    }
    if (g_persist_loaded && starts_with(name, "persist."))
        write_persistent(name, value);
    init_notify("set", name, value, 0);
    return PROP_SUCCESS;
}

// CheckPermissions without the SELinux check: the name, and the value
// against the type property_contexts gives the name.
static uint32_t check_permissions(const char *name, const char *value, const char **error)
{
    if (!legal_name(name)) { *error = "Illegal property name"; return PROP_ERROR_INVALID_NAME; }
    if (starts_with(name, "ctl.")) return PROP_SUCCESS;
    uint32_t ci, ti;
    pi_lookup(name, &ci, &ti);
    const char *type = ti == ~0u ? NULL : pi_type(ti);
    if (!check_type(type, value)) {
        static char msg[128];
        snprintf(msg, sizeof msg, "Property type check failed, value doesn't match expected type '%s'",
                 type ? type : "(null)");
        *error = msg;
        return PROP_ERROR_INVALID_VALUE;
    }
    return PROP_SUCCESS;
}

// HandlePropertySet. `pid` is the caller's (0 for the service itself).
static uint32_t handle_set(const char *name, const char *value, int pid, const char **error)
{
    uint32_t r = check_permissions(name, value, error);
    if (r != PROP_SUCCESS) return r;
    if (starts_with(name, "ctl.")) {
        // Android's init queues the message and answers success; whether the
        // service exists is its business (HandleControlMessage logs it).
        if (init_notify("ctl", name + 4, value, pid)) {
            plog("Received control message '%s' for '%s' from pid %d: passed to init", name + 4, value, pid);
            return PROP_SUCCESS;
        }
        // No init here to start, stop or restart a service.
        plog("Received control message '%s' for '%s' from pid %d: refused, no init to act on it",
             name + 4, value, pid);
        *error = "no init to handle control messages";
        return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
    }
    if (!strcmp(name, "sys.powerctl"))
        plog("Received sys.powerctl='%s' from pid: %d (no reboot or shutdown is done here)", value, pid);
    if (!strcmp(name, "selinux.restorecon_recursive") && pid != 0 && *value)
        plog("restorecon of '%s' requested by pid %d: no SELinux labels here, done", value, pid);
    return property_set(name, value, error);
}

static void init_set(const char *name, const char *value)
{
    const char *err = "";
    if (handle_set(name, value, 0, &err) != PROP_SUCCESS)
        plog("Init cannot set '%s' to '%s': %s", name, value, err);
}

// --------------------------------------------------- persistent properties
//
// init/persistent_properties.cpp's file: a PersistentProperties protobuf,
// `repeated PersistentPropertyRecord properties = 1` with `name = 1` and
// `value = 2`, rewritten whole through a .tmp file on every change.
struct kv { char *k, *v; };
struct kvlist { struct kv *a; int n; };

static void kv_set(struct kvlist *l, const char *k, const char *v, bool warn)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->a[i].k, k)) {
            if (warn && strcmp(l->a[i].v, v))
                plog("Overriding previous property '%s':'%s' with new value '%s'", k, l->a[i].v, v);
            free(l->a[i].v);
            l->a[i].v = xstrdup(v);
            return;
        }
    l->a = xrealloc(l->a, sizeof *l->a * (size_t)(l->n + 1));
    l->a[l->n++] = (struct kv){ xstrdup(k), xstrdup(v) };
}
static void kv_free(struct kvlist *l)
{
    for (int i = 0; i < l->n; i++) { free(l->a[i].k); free(l->a[i].v); }
    free(l->a);
    l->a = NULL;
    l->n = 0;
}

static bool pb_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    *v = 0;
    for (int sh = 0; sh < 64 && *p < end; sh += 7) {
        uint8_t c = *(*p)++;
        *v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) return true;
    }
    return false;
}
static bool pb_skip(const uint8_t **p, const uint8_t *end, int wt)
{
    uint64_t v;
    switch (wt) {
    case 0: return pb_varint(p, end, &v);
    case 1: if (end - *p < 8) return false; *p += 8; return true;
    case 2: if (!pb_varint(p, end, &v) || (uint64_t)(end - *p) < v) return false; *p += v; return true;
    case 5: if (end - *p < 4) return false; *p += 4; return true;
    default: return false;
    }
}

// LoadPersistentPropertyFile: false (and *out untouched) when the file is
// missing or unreadable; a file that does not parse is deleted, as init
// does, so the next write starts clean. A stale .tmp is removed first.
static bool load_persistent_file(struct kvlist *out)
{
    char tmpf[PATH_MAX + 8];
    snprintf(tmpf, sizeof tmpf, "%s.tmp", g_persist);
    if (unlink(tmpf) == 0)
        plog("Found temporary property file while attempting to persistent system properties"
             " a previous persistent property write may have failed");
    int fd = open(g_persist, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) { close(fd); return false; }
    uint8_t *d = xmalloc((size_t)st.st_size);
    ssize_t got = read(fd, d, (size_t)st.st_size);
    close(fd);
    if (got != st.st_size) { free(d); return false; }
    struct kvlist tmp = { 0 };
    const uint8_t *p = d, *end = d + got;
    bool ok = true;
    while (ok && p < end) {
        uint64_t key, len;
        if (!pb_varint(&p, end, &key)) { ok = false; break; }
        if (key != ((1 << 3) | 2)) { ok = pb_skip(&p, end, (int)(key & 7)); continue; }
        if (!pb_varint(&p, end, &len) || (uint64_t)(end - p) < len) { ok = false; break; }
        const uint8_t *r = p, *rend = p + len;
        p = rend;
        char *name = NULL, *value = NULL;
        while (ok && r < rend) {
            uint64_t k2, l2;
            if (!pb_varint(&r, rend, &k2)) { ok = false; break; }
            if ((k2 & 7) != 2 || ((k2 >> 3) != 1 && (k2 >> 3) != 2)) { ok = pb_skip(&r, rend, (int)(k2 & 7)); continue; }
            if (!pb_varint(&r, rend, &l2) || (uint64_t)(rend - r) < l2) { ok = false; break; }
            char *s = xmalloc(l2 + 1);
            memcpy(s, r, l2);
            s[l2] = '\0';
            r += l2;
            if ((k2 >> 3) == 1) { free(name); name = s; } else { free(value); value = s; }
        }
        if (ok) kv_set(&tmp, name ? name : "", value ? value : "", false);
        free(name);
        free(value);
    }
    free(d);
    if (!ok) {
        plog("Unable to parse persistent property file %s: Could not parse protobuf (deleted)", g_persist);
        unlink(g_persist);
        kv_free(&tmp);
        return false;
    }
    for (int i = 0; i < tmp.n; i++)
        kv_set(out, tmp.a[i].k, tmp.a[i].v, false);
    kv_free(&tmp);
    return true;
}

static void pb_put_varint(uint8_t **p, uint64_t v)
{
    while (v >= 0x80) { *(*p)++ = (uint8_t)(v | 0x80); v >>= 7; }
    *(*p)++ = (uint8_t)v;
}
static size_t varint_len(uint64_t v) { size_t n = 1; while (v >= 0x80) { v >>= 7; n++; } return n; }

static bool write_persistent_file(const struct kvlist *l)
{
    size_t cap = 16;
    for (int i = 0; i < l->n; i++) cap += strlen(l->a[i].k) + strlen(l->a[i].v) + 32;
    uint8_t *buf = xmalloc(cap), *p = buf;
    for (int i = 0; i < l->n; i++) {
        size_t kl = strlen(l->a[i].k), vl = strlen(l->a[i].v);
        size_t rl = 1 + varint_len(kl) + kl + 1 + varint_len(vl) + vl;
        *p++ = (1 << 3) | 2;
        pb_put_varint(&p, rl);
        *p++ = (1 << 3) | 2;
        pb_put_varint(&p, kl);
        memcpy(p, l->a[i].k, kl); p += kl;
        *p++ = (2 << 3) | 2;
        pb_put_varint(&p, vl);
        memcpy(p, l->a[i].v, vl); p += vl;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_persist);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write(fd, buf, (size_t)(p - buf)) == (ssize_t)(p - buf);
    if (fd >= 0) { fsync(fd); close(fd); }
    free(buf);
    if (ok && rename(tmp, g_persist) != 0) ok = false;
    if (!ok) { plog("Could not store persistent properties in %s: %s", g_persist, strerror(errno)); unlink(tmp); return false; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_persist);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        int dfd = open(dir, O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }
    return true;
}

static void collect_persist(const char *n, const char *v, void *ck)
{
    if (starts_with(n, "persist.")) kv_set(ck, n, v, false);
}

static void write_persistent(const char *name, const char *value)
{
    struct kvlist l = { 0 };
    if (!load_persistent_file(&l)) {
        plog("Recovering persistent properties from memory");
        kv_free(&l);
        prop_foreach(collect_persist, &l);
    }
    kv_set(&l, name, value, false);
    write_persistent_file(&l);
    kv_free(&l);
}

// ----------------------------------------------------- loading .prop files

static char *expand_props(const char *in)
{
    // init's ExpandProps: $$, ${name} and ${name:-default}, looked up in
    // what is set so far.
    size_t cap = strlen(in) + 256, n = 0;
    char *out = xmalloc(cap);
    for (const char *p = in; *p; ) {
        const char *add = NULL;
        size_t al = 0;
        if (p[0] == '$' && p[1] == '$') { add = "$"; al = 1; p += 2; }
        else if (p[0] == '$' && p[1] == '{') {
            const char *e = strchr(p, '}');
            if (!e) { plog("unterminated {: %s", in); free(out); return NULL; }
            char name[256];
            size_t nl = (size_t)(e - p - 2);
            if (nl >= sizeof name) { free(out); return NULL; }
            memcpy(name, p + 2, nl);
            name[nl] = '\0';
            const char *def = NULL;
            char *dd = strstr(name, ":-");
            if (dd) { *dd = '\0'; def = dd + 2; }
            const char *v = get_prop(name, NULL);
            if (!v || !*v) v = def;
            if (!v) { plog("property '%s' doesn't exist while expanding '%s'", name, in); free(out); return NULL; }
            add = v; al = strlen(v);            // the area outlives this call
            p = e + 1;
        } else if (p[0] == '$') { plog("unexpected end of string in '%s', looking for }", in); free(out); return NULL; }
        else { add = p; al = 1; p++; }
        if (n + al + 1 > cap) { cap = (n + al + 1) * 2; out = xrealloc(out, cap); }
        memcpy(out + n, add, al);
        n += al;
    }
    out[n] = '\0';
    return out;
}

static bool load_properties_from_file(const char *fname, const char *filter, struct kvlist *props);

// init's LoadProperties, including its in-place trimming.
static void load_properties(char *data, const char *filter, const char *fname, struct kvlist *props)
{
    size_t flen = filter ? strlen(filter) : 0;
    char *sol = data, *eol;
    while ((eol = strchr(sol, '\n'))) {
        char *key = sol;
        *eol++ = '\0';
        sol = eol;
        while (isspace((unsigned char)*key)) key++;
        if (*key == '#') continue;
        char *tmp = eol - 2;
        while (tmp > key && isspace((unsigned char)*tmp)) *tmp-- = '\0';
        if (!strncmp(key, "import ", 7) && flen == 0) {
            char *fn = key + 7;
            while (isspace((unsigned char)*fn)) fn++;
            key = strchr(fn, ' ');
            if (key) {
                *key++ = '\0';
                while (isspace((unsigned char)*key)) key++;
            }
            char *exp = expand_props(fn);
            if (!exp) { plog("Could not expand filename '%s'", fn); continue; }
            load_properties_from_file(exp, key, props);
            free(exp);
        } else {
            char *value = strchr(key, '=');
            if (!value) continue;
            *value++ = '\0';
            tmp = value - 2;
            while (tmp > key && isspace((unsigned char)*tmp)) *tmp-- = '\0';
            while (isspace((unsigned char)*value)) value++;
            if (flen > 0) {
                if (filter[flen - 1] == '*') { if (strncmp(key, filter, flen - 1) != 0) continue; }
                else if (strcmp(key, filter) != 0) continue;
            }
            if (starts_with(key, "ctl.") || !strcmp(key, "sys.powerctl") ||
                !strcmp(key, "selinux.restorecon_recursive")) {
                plog("Ignoring disallowed property '%s' with special meaning in prop file '%s'", key, fname);
                continue;
            }
            const char *err = "";
            if (check_permissions(key, value, &err) == PROP_SUCCESS)
                kv_set(props, key, value, true);
            else
                plog("Do not have permissions to set '%s' to '%s' in property file '%s': %s", key, value, fname, err);
        }
    }
}

static bool load_properties_from_file(const char *fname, const char *filter, struct kvlist *props)
{
    size_t len;
    char *d = read_guest_file(fname, &len);
    if (!d) {
        plog("Couldn't load property file '%s'", fname);
        return false;
    }
    d[len] = '\n';
    d[len + 1] = '\0';
    load_properties(d, filter, fname, props);
    free(d);
    return true;
}

static int cmp_kv(const void *a, const void *b) { return strcmp(((const struct kv *)a)->k, ((const struct kv *)b)->k); }

// property_initialize_ro_product_props.
static void derive_ro_product(void)
{
    static const char *const props[] = { "brand", "device", "manufacturer", "model", "name" };
    static const char *const allowed[] = { "odm", "product", "system_ext", "system", "vendor" };
    const char *deflt = "product,odm,vendor,system_ext,system";
    char *order = xstrdup(get_prop("ro.product.property_source_order", ""));
    if (order[0]) {
        // android::base::Split: empty pieces count (and are not allowed).
        char *tmp = xstrdup(order), *sp = tmp;
        for (char *s; (s = strsep(&sp, ",")); ) {
            bool ok = false;
            for (size_t i = 0; i < 5; i++) if (!strcmp(s, allowed[i])) ok = true;
            if (!ok) {
                plog("Found unexpected source in ro.product.property_source_order; using the default property source order");
                free(order);
                order = xstrdup(deflt);
                break;
            }
        }
        free(tmp);
    } else {
        free(order);
        order = xstrdup(deflt);
    }
    for (size_t i = 0; i < 5; i++) {
        char base[64];
        snprintf(base, sizeof base, "ro.product.%s", props[i]);
        if (*get_prop(base, "")) continue;
        char *tmp = xstrdup(order), *sp = tmp;
        for (char *s; (s = strsep(&sp, ",")); ) {
            char target[96];
            snprintf(target, sizeof target, "ro.product.%s.%s", s, props[i]);
            const char *v = get_prop(target, "");
            if (*v) {
                // A copy of any length: ro.* values may be long ones.
                char *val = xstrdup(v);
                plog("Setting product property %s to '%s' (from %s)", base, val, target);
                const char *err = "";
                uint32_t r = property_set(base, val, &err);
                if (r != PROP_SUCCESS) plog("Error setting product property %s: err=%u (%s)", base, r, err);
                free(val);
                break;
            }
        }
        free(tmp);
    }
    free(order);
}

// property_derive_build_fingerprint: only when the image does not set one,
// from the image's own values.
static void derive_fingerprint(void)
{
    if (*get_prop("ro.build.fingerprint", "")) return;
    char *fp = NULL;
    if (asprintf(&fp, "%s/%s/%s:%s/%s/%s:%s/%s",
                 get_prop("ro.product.brand", "unknown"), get_prop("ro.product.name", "unknown"),
                 get_prop("ro.product.device", "unknown"), get_prop("ro.build.version.release", "unknown"),
                 get_prop("ro.build.id", "unknown"), get_prop("ro.build.version.incremental", "unknown"),
                 get_prop("ro.build.type", "unknown"), get_prop("ro.build.tags", "unknown")) < 0)
        return;
    plog("Setting property 'ro.build.fingerprint' to '%s'", fp);
    const char *err = "";
    uint32_t r = property_set("ro.build.fingerprint", fp, &err);
    if (r != PROP_SUCCESS) plog("Error setting property 'ro.build.fingerprint': err=%u (%s)", r, err);
    free(fp);
}

// ProcessKernelCmdline + ExportKernelBootProps. There is no kernel command
// line: LXRT_PROPERTY_BOOTARGS stands for it ("androidboot.x=y ..."), empty
// by default, and "qemu" in it is not honoured (SteamARM is not the
// emulator).
static void boot_props(void)
{
    const char *args = getenv("LXRT_PROPERTY_BOOTARGS");
    if (args && *args) {
        char *copy = xstrdup(args), *sp = NULL;
        for (char *t = strtok_r(copy, " \t", &sp); t; t = strtok_r(NULL, " \t", &sp)) {
            char *eq = strchr(t, '=');
            if (!eq || !starts_with(t, "androidboot.")) continue;
            *eq = '\0';
            char name[128];
            snprintf(name, sizeof name, "ro.boot.%s", t + 12);
            init_set(name, eq + 1);
        }
        free(copy);
    }
    static const struct { const char *src, *dst, *def; } map[] = {
        { "ro.boot.serialno",   "ro.serialno",   "" },
        { "ro.boot.mode",       "ro.bootmode",   "unknown" },
        { "ro.boot.baseband",   "ro.baseband",   "unknown" },
        { "ro.boot.bootloader", "ro.bootloader", "unknown" },
        { "ro.boot.hardware",   "ro.hardware",   "unknown" },
        { "ro.boot.revision",   "ro.revision",   "0" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        char *v = xstrdup(get_prop(map[i].src, map[i].def));
        if (*v) init_set(map[i].dst, v);
        free(v);
    }
}

// PropertyLoadBootDefaults.
static void load_boot_defaults(void)
{
    struct kvlist props = { 0 };
    if (!load_properties_from_file("/system/etc/prop.default", NULL, &props))
        if (!load_properties_from_file("/prop.default", NULL, &props))
            load_properties_from_file("/default.prop", NULL, &props);
    load_properties_from_file("/system/build.prop", NULL, &props);
    load_properties_from_file("/system_ext/build.prop", NULL, &props);
    load_properties_from_file("/vendor/default.prop", NULL, &props);
    load_properties_from_file("/vendor/build.prop", NULL, &props);
    if (g_vendor_api >= 29) {
        load_properties_from_file("/odm/etc/build.prop", NULL, &props);
    } else {
        load_properties_from_file("/odm/default.prop", NULL, &props);
        load_properties_from_file("/odm/build.prop", NULL, &props);
    }
    load_properties_from_file("/product/build.prop", NULL, &props);
    load_properties_from_file("/factory/factory.prop", "ro.*", &props);
    if (guest_readable("/debug_ramdisk/adb_debug.prop"))
        load_properties_from_file("/debug_ramdisk/adb_debug.prop", NULL, &props);
    // LXRT_PROPERTY_HOST: the host's own .prop file (a host path), loaded
    // last among the image's files and with their rules -- what Waydroid's
    // container manager does with the waydroid.prop it writes for each boot.
    // scripts/android-boot.py puts there what this host can do that the
    // image cannot know, e.g. that it runs no 32-bit code:
    // ro.product.cpu.abilist32 empty (the framework otherwise waited for, and
    // tried to start processes in, a 32-bit zygote: benchmarks/stage28-
    // android-apk.txt).
    const char *hostprop = getenv("LXRT_PROPERTY_HOST");
    if (hostprop && *hostprop) {
        int fd = open(hostprop, O_RDONLY | O_CLOEXEC);
        struct stat hs;
        if (fd >= 0 && fstat(fd, &hs) == 0 && S_ISREG(hs.st_mode) && hs.st_uid == getuid() &&
            hs.st_size < (1 << 20)) {
            char *d = xmalloc((size_t)hs.st_size + 2);
            ssize_t got = read(fd, d, (size_t)hs.st_size);
            if (got < 0) got = 0;
            d[got] = '\n';
            d[got + 1] = '\0';
            load_properties(d, NULL, hostprop, &props);
            free(d);
            plog("host properties from %s", hostprop);
        } else {
            plog("LXRT_PROPERTY_HOST=%s: not a regular file of this user; ignored", hostprop);
        }
        if (fd >= 0) close(fd);
    }
    // std::map order.
    qsort(props.a, (size_t)props.n, sizeof *props.a, cmp_kv);
    int set = 0;
    for (int i = 0; i < props.n; i++) {
        const char *err = "";
        if (property_set(props.a[i].k, props.a[i].v, &err) != PROP_SUCCESS)
            plog("Could not set '%s' to '%s' while loading .prop files: %s", props.a[i].k, props.a[i].v, err);
        else set++;
    }
    plog("loaded %d properties from the image's .prop files (%d entries)", set, props.n);
    kv_free(&props);
    derive_ro_product();
    derive_fingerprint();
}

// init's legacy format, one file per property in the persistent file's
// directory (/data/property/persist.*), read when the protobuf file cannot
// be. init requires each to be root:root, closed to group and others and
// not a hard link; here the owner is the Mac user every file has.
static void legacy_dir(char *dir, size_t n)
{
    snprintf(dir, n, "%s", g_persist);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = '\0';
}

static bool load_legacy_persistent(struct kvlist *out)
{
    char dir[PATH_MAX];
    legacy_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) { plog("Unable to open persistent property directory \"%s\"", dir); return false; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!starts_with(de->d_name, "persist.") || de->d_type != DT_REG) continue;
        int fd = openat(dirfd(d), de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat sb;
        if (fstat(fd, &sb) != 0 || (sb.st_mode & (S_IRWXG | S_IRWXO)) || sb.st_uid != getuid() ||
            sb.st_nlink != 1 || sb.st_size > (1 << 20)) {
            plog("skipping insecure property file %s", de->d_name);
            close(fd);
            continue;
        }
        char *v = xmalloc((size_t)sb.st_size + 1);
        ssize_t got = read(fd, v, (size_t)sb.st_size);
        close(fd);
        if (got >= 0) { v[got] = '\0'; kv_set(out, de->d_name, v, false); }
        free(v);
    }
    closedir(d);
    return true;
}

static void remove_legacy_persistent(void)
{
    char dir[PATH_MAX];
    legacy_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)))
        if (starts_with(de->d_name, "persist.") && de->d_type == DT_REG)
            unlinkat(dirfd(d), de->d_name, 0);
    closedir(d);
}

// The persistent part of init's boot: /data/local.prop on a debuggable
// build (load_override_properties), then the stored persist.* values, then
// ro.persistent_properties.ready.
static void load_persistent(void)
{
    if (!strcmp(get_prop("ro.debuggable", "0"), "1")) {
        struct kvlist l = { 0 };
        if (guest_readable("/data/local.prop")) {
            load_properties_from_file("/data/local.prop", NULL, &l);
            qsort(l.a, (size_t)l.n, sizeof *l.a, cmp_kv);
            for (int i = 0; i < l.n; i++) {
                const char *err = "";
                if (property_set(l.a[i].k, l.a[i].v, &err) != PROP_SUCCESS)
                    plog("Could not set '%s' to '%s' in /data/local.prop: %s", l.a[i].k, l.a[i].v, err);
            }
        }
        kv_free(&l);
    }
    struct kvlist l = { 0 };
    if (!load_persistent_file(&l)) {
        plog("Could not load single persistent property file, trying legacy directory");
        if (load_legacy_persistent(&l)) {
            // Migrated as init migrates them: one protobuf file, then the
            // old per-property files go.
            if (write_persistent_file(&l)) remove_legacy_persistent();
        }
    }
    for (int i = 0; i < l.n; i++)
        init_set(l.a[i].k, l.a[i].v);
    plog("%d persistent properties from %s", l.n, g_persist);
    kv_free(&l);
    init_set("ro.persistent_properties.ready", "true");
    g_persist_loaded = true;
}

// ------------------------------------------------------- the area files

static uint8_t *map_area_rw(const char *path, bool create)
{
    int fd;
    if (create) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 && ftruncate(fd, PA_SIZE) != 0) { close(fd); fd = -1; }
    } else {
        // 0444 on disk, as init makes them: this process writes through a
        // mapping taken while the owner could still open it for writing.
        chmod(path, 0644);
        fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) { plog("%s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != PA_SIZE) {
        plog("%s: not a %d-byte property area", path, PA_SIZE);
        close(fd);
        return NULL;
    }
    uint8_t *m = mmap(NULL, PA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    fchmod(fd, 0444);
    close(fd);
    if (m == MAP_FAILED) { plog("%s: mmap: %s", path, strerror(errno)); return NULL; }
    if (create) {
        // The prop_area constructor: the root node and the dirty backup
        // area are reserved at the start of data_.
        *pa_bytes_used(m) = PROP_BT_HDR + ((PROP_VALUE_MAX + 3) & ~3);
        atomic_store_explicit(pa_serial(m), 0, memory_order_relaxed);
        memcpy(m + 8, &(uint32_t){ PROP_AREA_MAGIC }, 4);
        memcpy(m + 12, &(uint32_t){ PROP_AREA_VERSION }, 4);
    } else {
        uint32_t mg, vr;
        memcpy(&mg, m + 8, 4);
        memcpy(&vr, m + 12, 4);
        if (mg != PROP_AREA_MAGIC || vr != PROP_AREA_VERSION || *pa_bytes_used(m) > PA_DATA_SIZE) {
            plog("%s: bad magic or version", path);
            munmap(m, PA_SIZE);
            return NULL;
        }
    }
    return m;
}

static bool write_file(const char *path, const void *d, size_t n, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, d, n) == (ssize_t)n;
    fchmod(fd, mode);
    close(fd);
    return ok;
}

// Set up the areas from property_info's contexts: the files are created
// (fresh boot) or opened as they are (a service that starts again).
static bool open_areas(const char *pdir, bool create)
{
    g_nareas = pi_num_contexts();
    if (g_nareas == ~0u || g_nareas > 4096) { plog("property_info: bad context count"); return false; }
    g_areas = calloc(g_nareas, sizeof *g_areas);
    if (!g_areas) return false;
    char path[PATH_MAX];
    for (uint32_t i = 0; i < g_nareas; i++) {
        const char *ctx = pi_context(i);
        if (!*ctx || strchr(ctx, '/') || !strcmp(ctx, ".") || !strcmp(ctx, "..")) {
            plog("property_info: bad context name '%s'", ctx);
            return false;
        }
        g_areas[i].ctx = xstrdup(ctx);
        snprintf(path, sizeof path, "%s/%s", pdir, ctx);
        if (!(g_areas[i].base = map_area_rw(path, create))) return false;
    }
    snprintf(path, sizeof path, "%s/properties_serial", pdir);
    return (g_serial_area = map_area_rw(path, create)) != NULL;
}

static bool load_property_info_file(const char *path)
{
    size_t len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 24 || st.st_size > (8 << 20)) { close(fd); return false; }
    len = (size_t)st.st_size;
    g_pi = xmalloc(len);
    bool ok = read(fd, g_pi, len) == (ssize_t)len;
    close(fd);
    g_pi_size = (uint32_t)len;
    // PropertyInfoAreaFile::LoadPath's checks.
    return ok && pi_u32(4) <= 1 && pi_u32(8) == g_pi_size;
}

static int vendor_api_level(void)
{
    // SelinuxGetVendorAndroidVersion: plat_sepolicy_vers.txt, "30.0".
    char *d = read_guest_file("/vendor/etc/selinux/plat_sepolicy_vers.txt", NULL);
    int v = d ? atoi(d) : 0;
    free(d);
    return v > 0 ? v : 10000;
}

// Boot: property_info, the areas, and every value init would set before
// its first service starts. Built in __properties__.new and renamed into
// place, so a guest never sees half an area set.
static bool boot(void)
{
    char pdir[PATH_MAX], tmpdir[PATH_MAX], path[PATH_MAX];
    snprintf(pdir, sizeof pdir, "%s/__properties__", g_dir);
    snprintf(tmpdir, sizeof tmpdir, "%s/__properties__.new", g_dir);
    // A half-built directory from a service that died building it.
    DIR *old = opendir(tmpdir);
    if (old) {
        struct dirent *de;
        while ((de = readdir(old))) {
            if (de->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s", tmpdir, de->d_name);
            unlink(path);
        }
        closedir(old);
        rmdir(tmpdir);
    }
    if (mkdir(tmpdir, 0711) != 0) { plog("%s: %s", tmpdir, strerror(errno)); return false; }

    // CreateSerializedPropertyInfo.
    struct builder b = { 0 };
    b.root.name = xstrdup("root");
    b.root.ctx = strset_add(&b.ctxs, "u:object_r:default_prop:s0");
    b.root.type = strset_add(&b.types, "string");
    bool req = g_vendor_api >= 30, ok = true;
    if (!guest_readable("/system/etc/selinux/plat_property_contexts")) {
        plog("no /system/etc/selinux/plat_property_contexts in %s", g_root);
        return false;
    }
    parse_contexts(&b, "/system/etc/selinux/plat_property_contexts", req, &ok);
    if (guest_readable("/system_ext/etc/selinux/system_ext_property_contexts"))
        parse_contexts(&b, "/system_ext/etc/selinux/system_ext_property_contexts", req, &ok);
    if (guest_readable("/vendor/etc/selinux/vendor_property_contexts"))
        parse_contexts(&b, "/vendor/etc/selinux/vendor_property_contexts", req, &ok);
    else
        parse_contexts(&b, "/vendor/etc/selinux/nonplat_property_contexts", req, &ok);
    if (guest_readable("/product/etc/selinux/product_property_contexts"))
        parse_contexts(&b, "/product/etc/selinux/product_property_contexts", req, &ok);
    if (guest_readable("/odm/etc/selinux/odm_property_contexts"))
        parse_contexts(&b, "/odm/etc/selinux/odm_property_contexts", req, &ok);
    if (!ok) return false;
    uint32_t size;
    uint8_t *ser = serialize_trie(&b, &size);
    snprintf(path, sizeof path, "%s/property_info", tmpdir);
    if (!write_file(path, ser, size, 0444)) { plog("%s: %s", path, strerror(errno)); return false; }
    free(ser);
    if (!load_property_info_file(path)) { plog("cannot read back %s", path); return false; }
    plog("property_info: %u bytes, %u contexts", g_pi_size, pi_num_contexts());

    // __system_property_area_init: every context's area and the serial one.
    if (!open_areas(tmpdir, true)) return false;

    boot_props();
    load_boot_defaults();
    init_set("ro.property_service.version", "2");
    load_persistent();

    if (rename(tmpdir, pdir) != 0) { plog("rename %s: %s", tmpdir, strerror(errno)); return false; }
    return true;
}

// A service that starts again: the areas are the state, adopt them.
//
// Only this process writes the areas, but they are files a same-user process
// could have changed while no service ran: every object reachable from an
// area's root must lie inside its used bytes before this process follows
// offsets in it again.
static bool valid_prop_info(uint8_t *b, uint32_t off, uint32_t used)
{
    if (off % 4 || (uint64_t)off + PROP_INFO_HDR + 1 > used) return false;
    uint8_t *pi = pa_data(b) + off;
    if (!memchr(pi + PROP_INFO_HDR, 0, used - off - PROP_INFO_HDR)) return false;
    uint32_t serial = atomic_load_explicit(au32(pi), memory_order_relaxed);
    if (serial & LONG_FLAG) {
        uint32_t lo;
        memcpy(&lo, pi + LONG_OFFSET_AT, 4);
        if ((uint64_t)off + lo >= used || !memchr(pi + lo, 0, used - off - lo)) return false;
    } else if ((serial >> 24) >= PROP_VALUE_MAX || pi[4 + (serial >> 24)] != 0) {
        return false;
    }
    return true;
}

static bool valid_bt(uint8_t *b, uint32_t off, uint32_t used, bool root, uint32_t *budget)
{
    if (!*budget) return false;
    (*budget)--;
    if (off % 4 || (uint64_t)off + PROP_BT_HDR > used) return false;
    uint8_t *bt = pa_data(b) + off;
    if (!root) {
        // The root node is never constructed: its "name" is the first byte
        // of the dirty backup area.
        uint32_t nl;
        memcpy(&nl, bt, 4);
        if ((uint64_t)off + PROP_BT_HDR + nl + 1 > used || bt[PROP_BT_HDR + nl] != 0) return false;
    }
    uint32_t po = atomic_load_explicit(au32(bt + 4), memory_order_relaxed);
    if (po && !valid_prop_info(b, po, used)) return false;
    for (int f = 8; f <= 16; f += 4) {
        uint32_t o = atomic_load_explicit(au32(bt + f), memory_order_relaxed);
        if (o && !valid_bt(b, o, used, false, budget)) return false;
    }
    return true;
}

static bool valid_area(uint8_t *b)
{
    uint32_t used = *pa_bytes_used(b);
    uint32_t budget = PA_DATA_SIZE / PROP_BT_HDR;      // more nodes cannot fit: a cycle
    return used <= PA_DATA_SIZE && used >= PROP_BT_HDR + ((PROP_VALUE_MAX + 3) & ~3) &&
           valid_bt(b, 0, used, true, &budget);
}

static void close_areas(void)
{
    for (uint32_t i = 0; i < g_nareas; i++) {
        if (g_areas[i].base) munmap(g_areas[i].base, PA_SIZE);
        free(g_areas[i].ctx);
    }
    free(g_areas);
    g_areas = NULL;
    g_nareas = 0;
    if (g_serial_area) munmap(g_serial_area, PA_SIZE);
    g_serial_area = NULL;
    free(g_pi);
    g_pi = NULL;
    g_pi_size = 0;
}

static bool adopt(void)
{
    char pdir[PATH_MAX], path[PATH_MAX];
    snprintf(pdir, sizeof pdir, "%s/__properties__", g_dir);
    snprintf(path, sizeof path, "%s/property_info", pdir);
    if (!load_property_info_file(path)) return false;
    if (!open_areas(pdir, false)) return false;
    for (uint32_t i = 0; i < g_nareas; i++)
        if (!valid_area(g_areas[i].base)) {
            plog("%s/%s: an offset points outside the area", pdir, g_areas[i].ctx);
            return false;
        }
    g_persist_loaded = true;
    return true;
}

// ------------------------------------------------------------ the socket

static bool recv_fully(int fd, void *p, size_t n, int *timeout_ms)
{
    uint8_t *d = p;
    while (n > 0) {
        if (*timeout_ms <= 0) return false;
        struct pollfd pf = { fd, POLLIN, 0 };
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = poll(&pf, 1, *timeout_ms);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int el = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        *timeout_ms -= el > 0 ? el : (r < 0 && errno == EINTR ? 1 : 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        ssize_t k = recv(fd, d, n, MSG_DONTWAIT);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return false;
        d += k;
        n -= (size_t)k;
    }
    return true;
}

static bool recv_string(int fd, char **out, int *timeout_ms)
{
    uint32_t len;
    if (!recv_fully(fd, &len, 4, timeout_ms)) return false;
    if (len > 0xffff) { plog("sys_prop: RecvString asked to read huge string: %u", len); return false; }
    char *s = xmalloc(len + 1);
    if (len && !recv_fully(fd, s, len, timeout_ms)) { free(s); return false; }
    s[len] = '\0';
    if (strlen(s) != len) {
        // An embedded NUL: init keeps the std::string whole and the name
        // then fails IsLegalPropertyName; the value would be cut here, so
        // refuse it outright.
        free(s);
        return false;
    }
    *out = s;
    return true;
}

static void send_u32(int fd, uint32_t v)
{
    ssize_t r;
    do r = send(fd, &v, 4, 0); while (r < 0 && errno == EINTR);
}

// handle_property_set_fd.
static void handle_connection(int s, int timeout)
{
    struct xucred cr;
    socklen_t cl = sizeof cr;
    pid_t pid = 0;
    socklen_t pl = sizeof pid;
    if (getsockopt(s, SOL_LOCAL, LOCAL_PEERCRED, &cr, &cl) != 0 || cr.cr_uid != getuid()) {
        plog("sys_prop: connection from another user refused");
        return;
    }
    getsockopt(s, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl);
    // init: 2 s (kDefaultSocketTimeout) for the whole message.
    uint32_t cmd = 0;
    if (!recv_fully(s, &cmd, 4, &timeout)) {
        plog("sys_prop: error while reading command from the socket");
        send_u32(s, PROP_ERROR_READ_CMD);
        return;
    }
    if (cmd == PROP_MSG_SETPROP) {
        char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
        if (!recv_fully(s, name, sizeof name, &timeout) || !recv_fully(s, value, sizeof value, &timeout)) {
            plog("sys_prop(PROP_MSG_SETPROP): error while reading name/value from the socket");
            return;
        }
        name[PROP_NAME_MAX - 1] = '\0';
        value[PROP_VALUE_MAX - 1] = '\0';
        const char *err = "";
        uint32_t r = handle_set(name, value, pid, &err);
        if (r != PROP_SUCCESS)
            plog("Unable to set property '%s' from pid:%d: %s", name, (int)pid, err);
        else
            plog("set %s=%s (pid %d, legacy protocol)", name, value, (int)pid);
        return;     // the reply is the close (POLLHUP)
    }
    if (cmd == PROP_MSG_SETPROP2) {
        char *name = NULL, *value = NULL;
        if (!recv_string(s, &name, &timeout) || !recv_string(s, &value, &timeout)) {
            plog("sys_prop(PROP_MSG_SETPROP2): error while reading name/value from the socket");
            send_u32(s, PROP_ERROR_READ_DATA);
            free(name);
            return;
        }
        const char *err = "";
        uint32_t r = handle_set(name, value, pid, &err);
        if (r != PROP_SUCCESS)
            plog("Unable to set property '%s' from pid:%d: %s", name, (int)pid, err);
        else
            plog("set %s=%s (pid %d)", name, value, (int)pid);
        send_u32(s, r);
        free(name);
        free(value);
        return;
    }
    plog("sys_prop: invalid command %u", cmd);
    send_u32(s, PROP_ERROR_INVALID_CMD);
}

static void on_term(int sig) { (void)sig; g_stop = 1; }

int lxrt_property_service_main(int argc, char **argv)
{
    // argv: <lxrun> --property-service <dir> <root> [--daemon]
    if (argc < 4) {
        fprintf(stderr, "usage: lxrun --property-service <dir> <android-root> [--daemon]\n");
        return 2;
    }
    g_dir = argv[2];
    bool daemonize = argc > 4 && !strcmp(argv[4], "--daemon");
    if (!realpath(argv[3], g_root)) {
        fprintf(stderr, "property service: %s: %s\n", argv[3], strerror(errno));
        return 1;
    }
    struct stat dst;
    if (lstat(g_dir, &dst) != 0 || !S_ISDIR(dst.st_mode) || dst.st_uid != getuid() || (dst.st_mode & 077)) {
        fprintf(stderr, "property service: %s is not a private directory of this user\n", g_dir);
        return 1;
    }
    const char *idle_env = getenv("LXRT_PROPERTY_IDLE");
    int idle_s = idle_env && *idle_env ? atoi(idle_env) : 60;   // 0: never leave
    const char *pe = getenv("LXRT_PROPERTY_PERSIST");
    if (pe && *pe) snprintf(g_persist, sizeof g_persist, "%s", pe);
    else {
        char d[PATH_MAX];
        root_path("/data/property", d, sizeof d);
        mkdir(d, 0700);
        snprintf(g_persist, sizeof g_persist, "%s/persistent_properties", d);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    if (daemonize) {
        // The spawner gave us /dev/null: the log is where anything goes.
        char logp[PATH_MAX];
        snprintf(logp, sizeof logp, "%s/service.log", g_dir);
        struct stat lst;
        int lfl = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
        if (stat(logp, &lst) == 0 && lst.st_size > (1 << 20)) lfl |= O_TRUNC;
        int lfd = open(logp, lfl, 0600);
        if (lfd >= 0) { dup2(lfd, 2); close(lfd); }
    }
    setvbuf(stderr, NULL, _IOLBF, 0);

    // One service per directory: the lock is held for the service's life. A
    // service that is leaving still holds it for a moment: wait a little.
    char lockp[PATH_MAX];
    snprintf(lockp, sizeof lockp, "%s/service.lock", g_dir);
    int lk = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk < 0) { plog("%s: %s", lockp, strerror(errno)); return 1; }
    bool locked = false;
    for (int i = 0; i < 100 && !locked; i++) {    // up to 10 s
        if (flock(lk, LOCK_EX | LOCK_NB) == 0) locked = true;
        else usleep(100000);
    }
    if (!locked) { plog("another property service holds %s", lockp); return 1; }

    g_vendor_api = vendor_api_level();
    init_channel_open();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool adopted = false;
    char pdir[PATH_MAX];
    snprintf(pdir, sizeof pdir, "%s/__properties__", g_dir);
    struct stat pst;
    if (lstat(pdir, &pst) == 0) {
        adopted = S_ISDIR(pst.st_mode) && adopt();
        if (!adopted) {
            // Unusable state (no property_info, a bad area, not a
            // directory): set it aside -- guests that map it keep their
            // copy -- and boot afresh.
            char aside[PATH_MAX];
            snprintf(aside, sizeof aside, "%s/__properties__.bad-%d", g_dir, (int)getpid());
            plog("cannot adopt %s; moved to %s, booting again", pdir, aside);
            close_areas();
            if (rename(pdir, aside) != 0) {
                plog("rename %s: %s", pdir, strerror(errno));
                return 1;
            }
        }
    }
    if (!adopted && !boot()) {
        plog("boot failed: no properties");
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    plog("%s %s in %.1f ms (root %s, persistent file %s)", adopted ? "adopted" : "booted", g_dir,
         (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, g_root, g_persist);

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s/property_service", g_dir) >= (int)sizeof sa.sun_path) {
        plog("%s: socket path too long", g_dir);
        return 1;
    }
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { plog("socket: %s", strerror(errno)); return 1; }
    fcntl(ls, F_SETFD, FD_CLOEXEC);
    unlink(sa.sun_path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
        plog("bind %s: %s", sa.sun_path, strerror(errno));
        return 1;
    }
    chmod(sa.sun_path, 0600);

    if (daemonize) {
        // The spawner waits for this first process: once it exits, the areas
        // exist and the socket listens, and the service is nobody's child.
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) _exit(0);
        setsid();
    }
    char pidp[PATH_MAX], pidv[32];
    snprintf(pidp, sizeof pidp, "%s/service.pid", g_dir);
    int pn = snprintf(pidv, sizeof pidv, "%d\n", (int)getpid());
    write_file(pidp, pidv, (size_t)pn, 0600);
    plog("serving %s (pid %d, leaves after %d s idle)", sa.sun_path, (int)getpid(), idle_s);

    time_t last = time(NULL);
    while (!g_stop) {
        struct pollfd pf = { ls, POLLIN, 0 };
        int r = poll(&pf, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            plog("poll: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            if (idle_s > 0 && time(NULL) - last >= idle_s) {
                plog("idle for %d s: leaving (the areas stay; the next setprop starts a service again)", idle_s);
                break;
            }
            // The directory went away (a "reboot" by removing it): leave.
            struct stat st;
            if (stat(g_dir, &st) != 0) break;
            continue;
        }
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        fcntl(c, F_SETFD, FD_CLOEXEC);
        handle_connection(c, 2000);
        close(c);
        last = time(NULL);
    }
    // Nobody can connect once the name is gone; whoever did in the meantime
    // is still answered, within 2 s in all (a client that finds no socket
    // starts a new service, which waits up to 10 s for this one's lock, and
    // the runtime waits as long for this one to leave: props.c).
    unlink(sa.sun_path);
    fcntl(ls, F_SETFL, O_NONBLOCK);
    struct timespec d0, d1;
    clock_gettime(CLOCK_MONOTONIC, &d0);
    for (int c; (c = accept(ls, NULL, NULL)) >= 0; close(c)) {
        fcntl(c, F_SETFL, 0);
        clock_gettime(CLOCK_MONOTONIC, &d1);
        int left = 2000 - (int)((d1.tv_sec - d0.tv_sec) * 1000 + (d1.tv_nsec - d0.tv_nsec) / 1000000);
        if (left <= 0) continue;                // closed unanswered: its setprop fails
        handle_connection(c, left);
    }
    close(ls);
    unlink(pidp);
    plog("stopped");
    return 0;
}
