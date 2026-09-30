// The binder driver, as a host process (lxrun --binder-hub <dir>).
//
// Android's binder is a kernel driver. There is no Linux kernel here and no
// VM (AGENTS.md), and guest processes are separate macOS processes, so the
// driver's state has to live somewhere every process can reach: this hub.
// One per user, started on demand by the first guest that opens a binder
// device (runtime/binder.c), gone again once no process has one open.
//
// What it keeps is what drivers/android/binder.c keeps, with the same names
// where that helps a reader compare: procs (one per open of a device),
// threads, nodes (a process's own objects), refs (handles to another
// process's nodes), transactions with their stacks, work lists, death
// notifications, and each proc's receive buffer. The buffer is the guest's
// mmap of the device: a file the guest maps read-only and the hub maps
// read-write, so the hub copies each transaction straight into the
// receiver's buffer and hands it offsets into it, as the kernel does.
//
// It is single-threaded. Every message from a guest thread is processed to
// completion before the next, which gives the driver's locking for free. A
// guest thread that would sleep in BINDER_WRITE_READ is "parked": its result
// is simply not sent until work arrives for it.
//
// Deviations from the Linux driver, stated rather than hidden:
//   * no priorities or scheduler policy (min_priority is kept, not applied);
//   * no SELinux: a node that asks for the sender's security context
//     (FLAT_BINDER_FLAG_TXN_SECURITY_CTX) gets the label the sender's
//     runtime reports (LXRT_BINDER_SECCTX, default "u:r:unlabeled:s0");
//   * no freezer (BINDER_FREEZE is EINVAL, as on kernels before 5.11);
//   * poll readiness is per process, not per polling thread (see
//     proc_poll_wanted), which is exact for the one-poller processes
//     Android has (servicemanager, Looper-based services).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "binder.h"

// Linux errno values the protocol carries (the runtime returns them to the
// guest unchanged, so they are Linux numbers, never Darwin's).
#define L_EPERM   1
#define L_ESRCH   3
#define L_EINTR   4
#define L_EBADF   9
#define L_EAGAIN  11
#define L_ENOMEM  12
#define L_EFAULT  14
#define L_EBUSY   16
#define L_EINVAL  22
#define L_ENOSPC  28
#define L_EPROTO  71

#define ALIGN8(x) (((x) + 7) & ~(uint64_t)7)
#define SZ_4M (4u << 20)

// ------------------------------------------------------------------ lists

struct list { struct list *prev, *next; };
#define LIST_INIT(l) do { (l)->prev = (l)->next = (l); } while (0)
static bool list_empty(const struct list *l) { return l->next == l; }
static void list_add_tail(struct list *n, struct list *h)
{
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_add_before(struct list *n, struct list *at) { list_add_tail(n, at); }
static void list_del_init(struct list *n)
{
    if (n->next == n || !n->next) { n->prev = n->next = n; return; }
    n->prev->next = n->next; n->next->prev = n->prev; n->prev = n->next = n;
}
#define container_of(p, T, m) ((T *)(void *)((char *)(p) - offsetof(T, m)))
#define list_for_each_safe(pos, tmp, head) \
    for (pos = (head)->next, tmp = pos->next; pos != (head); pos = tmp, tmp = pos->next)

// ------------------------------------------------------------------ state

enum work_type {
    W_TRANSACTION = 1,
    W_TRANSACTION_COMPLETE,
    W_RETURN_ERROR,
    W_NODE,
    W_DEAD_BINDER,
    W_DEAD_BINDER_AND_CLEAR,
    W_CLEAR_DEATH_NOTIFICATION,
};
struct work { struct list entry; int type; };
struct error_work { struct work work; uint32_t cmd; };

enum {
    LOOPER_REGISTERED = 0x01,
    LOOPER_ENTERED    = 0x02,
    LOOPER_EXITED     = 0x04,
    LOOPER_INVALID    = 0x08,
    LOOPER_WAITING    = 0x10,
    LOOPER_POLL       = 0x20,
};

struct proc;
struct thread;
struct transaction;

struct node {
    struct work work;               // W_NODE: BR_INCREFS/ACQUIRE/RELEASE/DECREFS
    struct list proc_entry;         // proc->nodes, or g_dead_nodes
    struct proc *proc;              // NULL once the owner died
    struct list refs;               // struct ref.node_entry
    int internal_strong_refs, local_strong_refs, local_weak_refs;
    uint64_t ptr, cookie;
    bool has_strong_ref, pending_strong_ref, has_weak_ref, pending_weak_ref;
    bool has_async_transaction, accept_fds, txn_security_ctx;
    int min_priority;
    struct list async_todo;
    uint32_t debug_id;
};

struct ref_death { struct work work; uint64_t cookie; };

struct ref {
    struct list proc_entry;         // proc->refs, ascending desc
    struct list node_entry;         // node->refs
    struct proc *proc;
    struct node *node;
    uint32_t desc;
    int strong, weak;
    struct ref_death *death;
};

struct buffer {
    struct list entry;              // proc->buffers, ascending off
    uint64_t off, size;
    uint64_t data_size, offsets_size, extra_size;
    bool allow_user_free, async, clear_on_free;
    struct transaction *transaction;
    struct node *target_node;
};

struct transaction {
    struct work work;
    uint32_t debug_id;
    struct thread *from;
    struct transaction *from_parent, *to_parent;
    struct proc *to_proc;
    struct thread *to_thread;
    struct buffer *buffer;
    uint32_t code, flags;
    uint32_t sender_euid;
    bool need_reply;
    uint64_t security_ctx;          // guest address in the target, 0: none
    int nfix;                       // descriptors held for the receiver
    int *fix_fd;
    uint64_t *fix_off;              // where each goes in the target buffer
};

struct thread {
    struct list proc_entry;
    struct proc *proc;
    int fd;                         // thread channel
    int tid;
    uint32_t looper;
    bool looper_need_return, process_todo, is_dead;
    struct list todo;
    struct transaction *transaction_stack;
    struct error_work return_error, reply_error;
    // A BINDER_WRITE_READ waiting for work.
    bool parked;
    struct list waiting_entry;      // proc->waiting_threads (proc work only)
    uint64_t park_seq, park_write_consumed, park_read_avail, park_read_consumed;
    bool park_nonblock;
    // FDA descriptors to close in this thread's process (buffers it freed),
    // carried by its next result.
    int *closes; int ncloses, ccloses;
    int *held; int nheld, cheld;    // descriptors sent with the last result (send_result)
};

struct proc {
    struct list entry;              // g_procs
    int fd;                         // process channel
    bool hello;
    uint32_t id;
    int pid;
    int host_pid;               // LOCAL_PEERPID for channel verification
    uint32_t euid;
    int context;
    char secctx[64];
    struct list threads, nodes, refs, todo, delivered_death, waiting_threads;
    int max_threads, requested_threads, requested_threads_started;
    bool is_dead;
    uint8_t *map;                   // the receive buffer (hub's view)
    bool unmapped;                  // the guest unmapped it: allocate nothing more
    uint64_t map_size, map_host_size, user_base;
    int64_t free_async;
    struct list buffers;
    uint64_t bells, bells_drained;
};

static struct list g_procs;
static struct list g_dead_nodes;
static struct node *g_ctx_mgr[3];
static uint32_t g_ctx_mgr_uid[3];
static bool g_ctx_mgr_uid_set[3];
static uint32_t g_debug_id, g_proc_id;
static bool g_verbose;
static const char *g_dir;

static void hub_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hub_log(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[binder-hub %ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define user_error(...) hub_log("user error: " __VA_ARGS__)
#define debug(...) do { if (g_verbose) hub_log(__VA_ARGS__); } while (0)

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) { hub_log("out of memory"); _exit(70); }
    return p;
}

// ------------------------------------------------------------------ work

static void enqueue_work(struct work *w, struct list *target) { list_add_tail(&w->entry, target); }
static void dequeue_work(struct work *w) { list_del_init(&w->entry); }
static struct work *dequeue_head(struct list *l)
{
    if (list_empty(l)) return NULL;
    struct work *w = container_of(l->next, struct work, entry);
    list_del_init(&w->entry);
    return w;
}
static bool work_queued(struct work *w) { return w->entry.next && w->entry.next != &w->entry; }

static void enqueue_thread_work(struct thread *t, struct work *w)
{
    enqueue_work(w, &t->todo);
    t->process_todo = true;
}
// Work the thread handles after the rest (a sync TRANSACTION_COMPLETE, a
// node's first references): the thread does not wake for it alone.
static void enqueue_deferred_thread_work(struct thread *t, struct work *w)
{
    enqueue_work(w, &t->todo);
}

static bool available_for_proc_work(struct thread *t)
{
    return !t->transaction_stack && list_empty(&t->todo) &&
           (t->looper & (LOOPER_ENTERED | LOOPER_REGISTERED));
}
static bool has_work(struct thread *t, bool do_proc_work)
{
    return t->process_todo || t->looper_need_return ||
           (do_proc_work && !list_empty(&t->proc->todo));
}

// binder_select_thread_ilocked: a thread asleep waiting for process work.
static struct thread *select_thread(struct proc *p)
{
    if (list_empty(&p->waiting_threads)) return NULL;
    struct thread *t = container_of(p->waiting_threads.next, struct thread, waiting_entry);
    list_del_init(&t->waiting_entry);
    return t;
}

// ------------------------------------------------------------------ nodes

static struct node *get_node(struct proc *p, uint64_t ptr)
{
    struct list *e;
    for (e = p->nodes.next; e != &p->nodes; e = e->next) {
        struct node *n = container_of(e, struct node, proc_entry);
        if (n->ptr == ptr) return n;
    }
    return NULL;
}

static struct node *new_node(struct proc *p, const struct flat_binder_object *fp)
{
    struct node *n = xcalloc(1, sizeof *n);
    n->debug_id = ++g_debug_id;
    n->proc = p;
    n->ptr = fp ? fp->binder : 0;
    n->cookie = fp ? fp->cookie : 0;
    n->work.type = W_NODE;
    LIST_INIT(&n->work.entry);
    LIST_INIT(&n->refs);
    LIST_INIT(&n->async_todo);
    uint32_t flags = fp ? fp->flags : 0;
    n->min_priority = (int)(flags & FLAT_BINDER_FLAG_PRIORITY_MASK);
    n->accept_fds = (flags & FLAT_BINDER_FLAG_ACCEPTS_FDS) != 0;
    n->txn_security_ctx = (flags & FLAT_BINDER_FLAG_TXN_SECURITY_CTX) != 0;
    list_add_tail(&n->proc_entry, &p->nodes);
    debug("%d new node %u u%016llx c%016llx", p->pid, n->debug_id,
          (unsigned long long)n->ptr, (unsigned long long)n->cookie);
    return n;
}

static void free_node(struct node *n)
{
    list_del_init(&n->proc_entry);
    dequeue_work(&n->work);
    free(n);
}

// binder_inc_node_nilocked
static int inc_node(struct node *n, int strong, int internal, struct list *target_list)
{
    struct proc *p = n->proc;
    if (strong) {
        if (internal) {
            if (!target_list && n->internal_strong_refs == 0 &&
                !(p && n == g_ctx_mgr[p->context] && n->has_strong_ref)) {
                hub_log("invalid inc strong node for %u", n->debug_id);
                return -L_EINVAL;
            }
            n->internal_strong_refs++;
        } else {
            n->local_strong_refs++;
        }
        if (!n->has_strong_ref && target_list) {
            struct thread *t = container_of(target_list, struct thread, todo);
            dequeue_work(&n->work);
            enqueue_deferred_thread_work(t, &n->work);
        }
    } else {
        if (!internal)
            n->local_weak_refs++;
        if (!n->has_weak_ref && !work_queued(&n->work)) {
            if (!target_list) {
                hub_log("invalid inc weak node for %u", n->debug_id);
                return -L_EINVAL;
            }
            enqueue_work(&n->work, target_list);
        }
    }
    return 0;
}

// binder_dec_node_nilocked; true: the caller must free the node.
static bool dec_node_nilocked(struct node *n, int strong, int internal)
{
    struct proc *p = n->proc;
    if (strong) {
        if (internal) n->internal_strong_refs--;
        else n->local_strong_refs--;
        if (n->local_strong_refs || n->internal_strong_refs)
            return false;
    } else {
        if (!internal) n->local_weak_refs--;
        if (n->local_weak_refs || !list_empty(&n->refs))
            return false;
    }
    if (p && (n->has_strong_ref || n->has_weak_ref)) {
        if (!work_queued(&n->work))
            enqueue_work(&n->work, &p->todo);
    } else {
        if (list_empty(&n->refs) && !n->local_strong_refs && !n->local_weak_refs) {
            debug("%s node %u deleted", p ? "refless" : "dead", n->debug_id);
            return true;
        }
    }
    return false;
}
static void dec_node(struct node *n, int strong, int internal)
{
    if (dec_node_nilocked(n, strong, internal))
        free_node(n);
}

// ------------------------------------------------------------------ refs

static struct ref *get_ref(struct proc *p, uint32_t desc, bool need_strong)
{
    struct list *e;
    for (e = p->refs.next; e != &p->refs; e = e->next) {
        struct ref *r = container_of(e, struct ref, proc_entry);
        if (r->desc == desc) {
            if (need_strong && !r->strong) {
                user_error("%d tried to use weak ref %u as strong ref", p->pid, desc);
                return NULL;
            }
            return r;
        }
        if (r->desc > desc) break;
    }
    return NULL;
}

// binder_get_ref_for_node_olocked + creation
static struct ref *get_ref_for_node(struct proc *p, struct node *n)
{
    struct list *e;
    for (e = p->refs.next; e != &p->refs; e = e->next) {
        struct ref *r = container_of(e, struct ref, proc_entry);
        if (r->node == n) return r;
    }
    struct ref *r = xcalloc(1, sizeof *r);
    r->proc = p;
    r->node = n;
    // The lowest free descriptor; 0 is the context manager's alone.
    uint32_t desc = (n == g_ctx_mgr[p->context]) ? 0 : 1;
    struct list *at = &p->refs;
    for (e = p->refs.next; e != &p->refs; e = e->next) {
        struct ref *o = container_of(e, struct ref, proc_entry);
        if (o->desc > desc) { at = e; break; }
        desc = o->desc + 1;
    }
    r->desc = desc;
    list_add_before(&r->proc_entry, at);
    list_add_tail(&r->node_entry, &n->refs);
    debug("%d new ref desc %u for node %u", p->pid, desc, n->debug_id);
    return r;
}

static int inc_ref(struct ref *r, int strong, struct list *target_list)
{
    int ret;
    if (strong) {
        if (r->strong == 0 && (ret = inc_node(r->node, 1, 1, target_list)))
            return ret;
        r->strong++;
    } else {
        if (r->weak == 0 && (ret = inc_node(r->node, 0, 1, target_list)))
            return ret;
        r->weak++;
    }
    return 0;
}

// binder_cleanup_ref_olocked + binder_free_ref
static void cleanup_ref(struct ref *r)
{
    struct node *n = r->node;
    list_del_init(&r->proc_entry);
    if (r->strong)
        dec_node_nilocked(n, 1, 1);     // the weak path below decides deletion
    list_del_init(&r->node_entry);
    bool del = dec_node_nilocked(n, 0, 1);
    if (r->death) {
        dequeue_work(&r->death->work);
        free(r->death);
    }
    free(r);
    if (del)
        free_node(n);
}

// true: the ref was deleted
static bool dec_ref(struct ref *r, int strong)
{
    if (strong) {
        if (r->strong == 0) {
            user_error("%d invalid dec strong, ref %u", r->proc->pid, r->desc);
            return false;
        }
        r->strong--;
        if (r->strong == 0)
            dec_node(r->node, 1, 1);
    } else {
        if (r->weak == 0) {
            user_error("%d invalid dec weak, ref %u", r->proc->pid, r->desc);
            return false;
        }
        r->weak--;
    }
    if (r->strong == 0 && r->weak == 0) {
        cleanup_ref(r);
        return true;
    }
    return false;
}

static int inc_ref_for_node(struct proc *p, struct node *n, bool strong,
                            struct list *target_list, uint32_t *desc)
{
    struct ref *r = get_ref_for_node(p, n);
    int ret = inc_ref(r, strong, target_list);
    *desc = r->desc;
    if (ret && r->strong == 0 && r->weak == 0)
        cleanup_ref(r);
    return ret;
}

static int update_ref_for_handle(struct proc *p, uint32_t desc, bool increment, bool strong)
{
    struct ref *r = get_ref(p, desc, strong);
    if (!r) return -L_EINVAL;
    if (increment) return inc_ref(r, strong, NULL);
    dec_ref(r, strong);
    return 0;
}

// ------------------------------------------------------------------ buffers

static struct buffer *alloc_buf(struct proc *p, uint64_t size, bool async, int *err)
{
    if (!p->map || p->unmapped) {       // binder_alloc_new_buf_locked: "no vma"
        user_error("%d: binder_alloc_buf, no vma", p->pid);
        *err = -L_ESRCH;
        return NULL;
    }
    size = ALIGN8(size < 8 ? 8 : size);
    if (async && p->free_async < (int64_t)(size + 64)) {
        user_error("%d: no async space left", p->pid);
        *err = -L_ENOSPC;
        return NULL;
    }
    uint64_t at = 0;
    struct list *e, *before = &p->buffers;
    for (e = p->buffers.next; e != &p->buffers; e = e->next) {
        struct buffer *b = container_of(e, struct buffer, entry);
        if (b->off - at >= size) { before = e; break; }
        at = b->off + b->size;
    }
    if (at + size > p->map_size) {
        user_error("%d: binder_alloc_buf size %llu failed, no address space",
                   p->pid, (unsigned long long)size);
        *err = -L_ENOSPC;
        return NULL;
    }
    struct buffer *b = xcalloc(1, sizeof *b);
    b->off = at;
    b->size = size;
    b->async = async;
    if (async) p->free_async -= (int64_t)(size + 64);
    list_add_before(&b->entry, before);
    return b;
}

static void free_buf_mem(struct proc *p, struct buffer *b)
{
    if (b->async) p->free_async += (int64_t)(b->size + 64);
    list_del_init(&b->entry);
    free(b);
}

static struct buffer *buffer_at_user(struct proc *p, uint64_t user)
{
    if (!p->map || user < p->user_base) return NULL;
    uint64_t off = user - p->user_base;
    struct list *e;
    for (e = p->buffers.next; e != &p->buffers; e = e->next) {
        struct buffer *b = container_of(e, struct buffer, entry);
        if (b->off == off) return b;
    }
    return NULL;
}

// binder_get_object: the size of a valid object at `off` in the buffer's data.
static size_t get_object(struct proc *p, struct buffer *b, uint64_t off, void *out)
{
    uint64_t left = b->data_size >= off ? b->data_size - off : 0;
    if (off > b->data_size || left < 4 || (off & 3))
        return 0;
    uint8_t *base = p->map + b->off;
    size_t rd = left < 40 ? left : 40;
    memcpy(out, base + off, rd);
    size_t sz;
    switch (((struct binder_object_header *)out)->type) {
    case BINDER_TYPE_BINDER: case BINDER_TYPE_WEAK_BINDER:
    case BINDER_TYPE_HANDLE: case BINDER_TYPE_WEAK_HANDLE:
        sz = sizeof(struct flat_binder_object); break;
    case BINDER_TYPE_FD: sz = sizeof(struct binder_fd_object); break;
    case BINDER_TYPE_PTR: sz = sizeof(struct binder_buffer_object); break;
    case BINDER_TYPE_FDA: sz = sizeof(struct binder_fd_array_object); break;
    default: return 0;
    }
    return (b->data_size >= sz && off <= b->data_size - sz) ? sz : 0;
}

static uint64_t buf_u64(struct proc *p, struct buffer *b, uint64_t off)
{
    uint64_t v;
    memcpy(&v, p->map + b->off + off, 8);
    return v;
}

// binder_validate_ptr
static bool validate_ptr(struct proc *p, struct buffer *b, uint64_t index,
                         uint64_t start, uint64_t num_valid,
                         struct binder_buffer_object *out, uint64_t *obj_off)
{
    if (index >= num_valid) return false;
    uint64_t o = buf_u64(p, b, start + index * 8);
    uint8_t obj[40];
    if (get_object(p, b, o, obj) != sizeof(struct binder_buffer_object)) return false;
    memcpy(out, obj, sizeof *out);
    if (out->hdr.type != BINDER_TYPE_PTR) return false;
    if (obj_off) *obj_off = o;
    return true;
}

// binder_validate_fixup
static bool validate_fixup(struct proc *p, struct buffer *b, uint64_t start,
                           uint64_t buffer_obj_off, uint64_t fixup_off,
                           uint64_t last_obj_off, uint64_t last_min_off)
{
    if (!last_obj_off) return false;
    while (last_obj_off != buffer_obj_off) {
        uint8_t obj[40];
        if (get_object(p, b, last_obj_off, obj) != sizeof(struct binder_buffer_object))
            return false;
        struct binder_buffer_object bbo;
        memcpy(&bbo, obj, sizeof bbo);
        if (!(bbo.flags & BINDER_BUFFER_FLAG_HAS_PARENT)) return false;
        last_min_off = bbo.parent_offset + 8;
        last_obj_off = buf_u64(p, b, start + 8 * bbo.parent);
    }
    return fixup_off >= last_min_off;
}

static void thread_add_close(struct thread *t, int fd)
{
    if (!t) return;
    if (t->ncloses == t->ccloses) {
        t->ccloses = t->ccloses ? t->ccloses * 2 : 16;
        t->closes = realloc(t->closes, (size_t)t->ccloses * sizeof(int));
        if (!t->closes) { hub_log("out of memory"); _exit(70); }
    }
    t->closes[t->ncloses++] = fd;
}

// binder_transaction_buffer_release: undo what a buffer's objects hold, in
// the process that owns the buffer. failed_at: offsets-array position the
// translation stopped at (0: all). FDA descriptors the receiver installed are
// closed there (the result to `t` carries them).
static void buffer_release(struct proc *p, struct thread *t, struct buffer *b,
                           uint64_t failed_at, bool is_failure)
{
    uint64_t start = ALIGN8(b->data_size);
    uint64_t end = (is_failure && failed_at) ? failed_at : start + b->offsets_size;
    if (b->target_node) {
        dec_node(b->target_node, 1, 0);
        b->target_node = NULL;
    }
    for (uint64_t bo = start; bo < end; bo += 8) {
        uint64_t oo = buf_u64(p, b, bo);
        uint8_t obj[40];
        size_t sz = get_object(p, b, oo, obj);
        if (!sz) {
            hub_log("transaction release: bad object at offset %llu", (unsigned long long)oo);
            continue;
        }
        uint32_t type = ((struct binder_object_header *)obj)->type;
        switch (type) {
        case BINDER_TYPE_BINDER: case BINDER_TYPE_WEAK_BINDER: {
            struct flat_binder_object fp;
            memcpy(&fp, obj, sizeof fp);
            struct node *n = get_node(p, fp.binder);
            if (!n) { hub_log("transaction release: bad node %016llx", (unsigned long long)fp.binder); break; }
            dec_node(n, type == BINDER_TYPE_BINDER, 0);
            break;
        }
        case BINDER_TYPE_HANDLE: case BINDER_TYPE_WEAK_HANDLE: {
            struct flat_binder_object fp;
            memcpy(&fp, obj, sizeof fp);
            struct ref *r = get_ref(p, fp.handle, type == BINDER_TYPE_HANDLE);
            if (!r) { hub_log("transaction release: bad handle %u", fp.handle); break; }
            dec_ref(r, type == BINDER_TYPE_HANDLE);
            break;
        }
        case BINDER_TYPE_FDA: {
            if (is_failure) break;
            struct binder_fd_array_object fda;
            memcpy(&fda, obj, sizeof fda);
            struct binder_buffer_object parent;
            uint64_t num_valid = (bo - start) / 8;
            if (!validate_ptr(p, b, fda.parent, start, num_valid, &parent, NULL)) {
                hub_log("transaction release: bad FDA parent");
                break;
            }
            // binder_transaction_buffer_release's checks, before any offset
            // is followed: the number of fds against the parent's length,
            // and the array inside this buffer (computed without wrapping).
            uint64_t ubase = p->user_base + b->off;
            if (fda.num_fds >= (1ull << 60) || 4 * fda.num_fds > parent.length ||
                fda.parent_offset > parent.length - 4 * fda.num_fds ||
                parent.buffer < ubase || parent.buffer - ubase > b->size ||
                fda.parent_offset > b->size - (parent.buffer - ubase) ||
                4 * fda.num_fds > b->size - (parent.buffer - ubase) - fda.parent_offset) {
                hub_log("transaction release: bad FDA");
                break;
            }
            uint64_t fda_off = parent.buffer - ubase + fda.parent_offset;
            for (uint64_t i = 0; i < fda.num_fds; i++) {
                uint64_t o = fda_off + i * 4;
                int32_t fd;
                memcpy(&fd, p->map + b->off + o, 4);
                thread_add_close(t, fd);
                if (t) t->looper_need_return = true;
            }
            break;
        }
        default:
            break;      // FD: userspace closes it; PTR: goes with the buffer
        }
    }
}

static void close_fixups(struct transaction *t)
{
    for (int i = 0; i < t->nfix; i++)
        if (t->fix_fd[i] >= 0) close(t->fix_fd[i]);
    free(t->fix_fd);
    free(t->fix_off);
    t->fix_fd = NULL;
    t->fix_off = NULL;
    t->nfix = 0;
}

static void free_transaction(struct transaction *t)
{
    if (t->buffer)
        t->buffer->transaction = NULL;
    close_fixups(t);
    free(t);
}

// ------------------------------------------------------------------ errors

static void send_failed_reply(struct transaction *t, uint32_t error_code);

static void pop_transaction(struct thread *target, struct transaction *t)
{
    if (target->transaction_stack != t || t->from != target) {
        hub_log("pop_transaction: stack mismatch");
        return;
    }
    target->transaction_stack = t->from_parent;
    t->from = NULL;
}

// binder_send_failed_reply
static void send_failed_reply(struct transaction *t, uint32_t error_code)
{
    while (1) {
        struct thread *target = t->from;
        if (target) {
            pop_transaction(target, t);
            if (target->reply_error.cmd == BR_OK) {
                target->reply_error.cmd = error_code;
                enqueue_thread_work(target, &target->reply_error.work);
            } else {
                hub_log("unexpected reply error: %u", target->reply_error.cmd);
            }
            free_transaction(t);
            return;
        }
        struct transaction *next = t->from_parent;
        free_transaction(t);
        if (!next) return;
        t = next;
    }
}

static void cleanup_transaction(struct transaction *t, uint32_t error_code)
{
    if (t->buffer && t->buffer->target_node && !(t->flags & TF_ONE_WAY))
        send_failed_reply(t, error_code);
    else
        free_transaction(t);
}

// binder_release_work
static void release_work(struct list *l)
{
    struct work *w;
    while ((w = dequeue_head(l))) {
        switch (w->type) {
        case W_TRANSACTION:
            cleanup_transaction(container_of(w, struct transaction, work), BR_DEAD_REPLY);
            break;
        case W_TRANSACTION_COMPLETE:
            free(w);
            break;
        case W_DEAD_BINDER_AND_CLEAR:
        case W_CLEAR_DEATH_NOTIFICATION:
            free(container_of(w, struct ref_death, work));
            break;
        default:
            break;      // node work, return errors: embedded, nothing to free
        }
    }
}

// ------------------------------------------------------------------ transactions

// What the sender attached for one transaction (see struct bh_txn_att).
struct att {
    int fault;
    const uint8_t *data, *offsets, *sg;
    uint64_t data_size, offsets_size, sg_size;
    int *fds;
    int nfds;
    int used;           // descriptors taken so far
};

static void set_return_error(struct thread *th, uint32_t cmd)
{
    th->return_error.cmd = cmd;
    enqueue_thread_work(th, &th->return_error.work);
}

static int add_fixup(struct transaction *t, struct att *a, uint64_t off, bool allowed,
                     struct thread *th)
{
    if (!allowed) {
        user_error("%d:%d got transaction with fd, but target does not allow fds",
                   th->proc->pid, th->tid);
        return -L_EPERM;
    }
    if (a->used >= a->nfds) {
        user_error("%d:%d got transaction with invalid fd", th->proc->pid, th->tid);
        return -L_EBADF;
    }
    int fd = a->fds[a->used];
    a->fds[a->used++] = -1;             // the transaction holds it now
    t->fix_fd = realloc(t->fix_fd, (size_t)(t->nfix + 1) * sizeof(int));
    t->fix_off = realloc(t->fix_off, (size_t)(t->nfix + 1) * sizeof(uint64_t));
    if (!t->fix_fd || !t->fix_off) { hub_log("out of memory"); _exit(70); }
    t->fix_fd[t->nfix] = fd;
    t->fix_off[t->nfix] = off;
    t->nfix++;
    return 0;
}

static bool proc_transaction(struct transaction *t, struct proc *p, struct thread *th);

// binder_transaction
static void transaction(struct thread *th, const struct binder_transaction_data *tr,
                        bool reply, uint64_t extra_buffers_size, struct att *a)
{
    struct proc *proc = th->proc;
    struct proc *target_proc = NULL;
    struct thread *target_thread = NULL;
    struct node *target_node = NULL;
    struct transaction *in_reply_to = NULL;
    uint32_t return_error = BR_OK;
    int param = 0;
    const char *why = "";

    if (reply) {
        in_reply_to = th->transaction_stack;
        if (!in_reply_to) {
            why = "got reply transaction with no transaction stack";
            return_error = BR_FAILED_REPLY; param = -L_EPROTO; goto err_early;
        }
        if (in_reply_to->to_thread != th) {
            why = "got reply transaction with bad transaction stack";
            return_error = BR_FAILED_REPLY; param = -L_EPROTO; in_reply_to = NULL; goto err_early;
        }
        th->transaction_stack = in_reply_to->to_parent;
        target_thread = in_reply_to->from;
        if (!target_thread) {
            return_error = BR_DEAD_REPLY; why = "reply target died"; goto err_early;
        }
        if (target_thread->transaction_stack != in_reply_to) {
            why = "got reply transaction with bad target transaction stack";
            return_error = BR_FAILED_REPLY; param = -L_EPROTO;
            in_reply_to = NULL; target_thread = NULL; goto err_early;
        }
        target_proc = target_thread->proc;
    } else {
        if (tr->target.handle) {
            struct ref *r = get_ref(proc, tr->target.handle, true);
            if (r) {
                if (r->node->proc) {
                    target_node = r->node;
                    inc_node(target_node, 1, 0, NULL);
                    target_proc = target_node->proc;
                } else {
                    return_error = BR_DEAD_REPLY;
                }
            } else {
                user_error("%d:%d got transaction to invalid handle %u",
                           proc->pid, th->tid, tr->target.handle);
                return_error = BR_FAILED_REPLY;
            }
        } else {
            target_node = g_ctx_mgr[proc->context];
            if (target_node && target_node->proc) {
                inc_node(target_node, 1, 0, NULL);
                target_proc = target_node->proc;
            } else {
                target_node = NULL;
                return_error = BR_DEAD_REPLY;
            }
            if (target_node && target_proc == proc) {
                why = "got transaction to context manager from process owning it";
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_node;
            }
        }
        if (!target_node) { param = -L_EINVAL; why = "no target"; goto err_early; }
        if (proc == target_proc) {
            why = "transaction to its own process";
            return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_node;
        }
        if (!(tr->flags & TF_ONE_WAY) && th->transaction_stack) {
            struct transaction *tmp = th->transaction_stack;
            if (tmp->to_thread != th) {
                why = "got new transaction with bad transaction stack";
                return_error = BR_FAILED_REPLY; param = -L_EPROTO; goto err_node;
            }
            while (tmp) {
                if (tmp->from && tmp->from->proc == target_proc)
                    target_thread = tmp->from;
                tmp = tmp->from_parent;
            }
        }
    }

    if (a->fault) {
        why = "got transaction with invalid data ptr";
        return_error = BR_FAILED_REPLY; param = -a->fault; goto err_node;
    }
    if (a->data_size != tr->data_size || a->offsets_size != tr->offsets_size) {
        why = "attachment does not match the transaction";
        return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_node;
    }
    if (tr->offsets_size % 8) {
        why = "got transaction with invalid offsets size";
        return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_node;
    }
    if (extra_buffers_size % 8) {
        why = "got transaction with unaligned buffers size";
        return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_node;
    }

    struct transaction *t = xcalloc(1, sizeof *t);
    t->debug_id = ++g_debug_id;
    t->work.type = W_TRANSACTION;
    LIST_INIT(&t->work.entry);
    struct work *tcomplete = xcalloc(1, sizeof *tcomplete);
    tcomplete->type = W_TRANSACTION_COMPLETE;
    LIST_INIT(&tcomplete->entry);

    t->from = (!reply && !(tr->flags & TF_ONE_WAY)) ? th : NULL;
    t->sender_euid = proc->euid;
    t->to_proc = target_proc;
    t->to_thread = target_thread;
    t->code = tr->code;
    t->flags = tr->flags;

    uint64_t secctx_sz = 0;
    const char *secctx = NULL;
    if (target_node && target_node->txn_security_ctx) {
        secctx = proc->secctx;
        secctx_sz = strlen(secctx) + 1;
        extra_buffers_size += ALIGN8(secctx_sz);
    }

    int aerr = 0;
    uint64_t total = ALIGN8(tr->data_size) + ALIGN8(tr->offsets_size) + ALIGN8(extra_buffers_size);
    if (total < tr->data_size || total > target_proc->map_size + 8) {
        return_error = BR_FAILED_REPLY; param = -L_ENOSPC; why = "transaction too large";
        goto err_t;
    }
    struct buffer *b = alloc_buf(target_proc, total, !reply && (t->flags & TF_ONE_WAY), &aerr);
    if (!b) {
        return_error = aerr == -L_ESRCH ? BR_DEAD_REPLY : BR_FAILED_REPLY;
        param = aerr; why = "binder_alloc_buf failed";
        goto err_t;
    }
    b->data_size = tr->data_size;
    b->offsets_size = tr->offsets_size;
    b->extra_size = extra_buffers_size;
    b->transaction = t;
    b->target_node = target_node;
    b->clear_on_free = (t->flags & TF_CLEAR_BUF) != 0;
    t->buffer = b;
    uint8_t *base = target_proc->map + b->off;
    uint64_t user = target_proc->user_base + b->off;
    if (secctx) {
        uint64_t so = ALIGN8(tr->data_size) + ALIGN8(tr->offsets_size) +
                      ALIGN8(extra_buffers_size) - ALIGN8(secctx_sz);
        memcpy(base + so, secctx, secctx_sz);
        t->security_ctx = user + so;
    }
    memcpy(base, a->data, tr->data_size);
    memcpy(base + ALIGN8(tr->data_size), a->offsets, tr->offsets_size);

    uint64_t off_start = ALIGN8(tr->data_size);
    uint64_t off_end = off_start + tr->offsets_size;
    uint64_t sg_off = ALIGN8(off_end);
    uint64_t sg_end = sg_off + extra_buffers_size - ALIGN8(secctx_sz);
    uint64_t sg_src = 0;                 // position in the sender's sg bytes
    uint64_t off_min = 0, last_fixup_obj_off = 0, last_fixup_min_off = 0;
    uint64_t bo;
    bool fds_ok = reply ? (in_reply_to->flags & TF_ACCEPT_FDS) != 0 : target_node->accept_fds;
    for (bo = off_start; bo < off_end; bo += 8) {
        uint64_t oo = buf_u64(target_proc, b, bo);
        uint8_t obj[40];
        size_t osz = get_object(target_proc, b, oo, obj);
        if (!osz || oo < off_min) {
            user_error("%d:%d got transaction with invalid offset (%lld, min %lld max %lld) or object",
                       proc->pid, th->tid, (long long)oo, (long long)off_min, (long long)b->data_size);
            return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
        }
        off_min = oo + osz;
        uint32_t type = ((struct binder_object_header *)obj)->type;
        switch (type) {
        case BINDER_TYPE_BINDER: case BINDER_TYPE_WEAK_BINDER: {
            // binder_translate_binder
            struct flat_binder_object fp;
            memcpy(&fp, obj, sizeof fp);
            struct node *n = get_node(proc, fp.binder);
            if (!n) n = new_node(proc, &fp);
            if (fp.cookie != n->cookie) {
                user_error("%d:%d sending u%016llx node %u, cookie mismatch %016llx != %016llx",
                           proc->pid, th->tid, (unsigned long long)fp.binder, n->debug_id,
                           (unsigned long long)fp.cookie, (unsigned long long)n->cookie);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            uint32_t desc;
            int r = inc_ref_for_node(target_proc, n, type == BINDER_TYPE_BINDER, &th->todo, &desc);
            if (r) { return_error = BR_FAILED_REPLY; param = r; goto err_translate; }
            fp.hdr.type = type == BINDER_TYPE_BINDER ? BINDER_TYPE_HANDLE : BINDER_TYPE_WEAK_HANDLE;
            fp.binder = 0;
            fp.handle = desc;
            fp.cookie = 0;
            memcpy(base + oo, &fp, sizeof fp);
            break;
        }
        case BINDER_TYPE_HANDLE: case BINDER_TYPE_WEAK_HANDLE: {
            // binder_translate_handle
            struct flat_binder_object fp;
            memcpy(&fp, obj, sizeof fp);
            struct ref *r = get_ref(proc, fp.handle, type == BINDER_TYPE_HANDLE);
            if (!r) {
                user_error("%d:%d got transaction with invalid handle, %d",
                           proc->pid, th->tid, (int)fp.handle);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            struct node *n = r->node;
            if (n->proc == target_proc) {
                fp.hdr.type = type == BINDER_TYPE_HANDLE ? BINDER_TYPE_BINDER : BINDER_TYPE_WEAK_BINDER;
                fp.binder = n->ptr;
                fp.cookie = n->cookie;
                inc_node(n, fp.hdr.type == BINDER_TYPE_BINDER, 0, NULL);
            } else {
                uint32_t desc;
                int rr = inc_ref_for_node(target_proc, n, type == BINDER_TYPE_HANDLE, NULL, &desc);
                if (rr) { return_error = BR_FAILED_REPLY; param = rr; goto err_translate; }
                fp.binder = 0;
                fp.handle = desc;
                fp.cookie = 0;
            }
            memcpy(base + oo, &fp, sizeof fp);
            break;
        }
        case BINDER_TYPE_FD: {
            struct binder_fd_object fo;
            memcpy(&fo, obj, sizeof fo);
            int r = add_fixup(t, a, oo + offsetof(struct binder_fd_object, fd), fds_ok, th);
            if (r) { return_error = BR_FAILED_REPLY; param = r; goto err_translate; }
            fo.pad_binder = 0;
            fo.fd = 0xffffffffu;        // the receiver's runtime writes the new fd here
            memcpy(base + oo, &fo, sizeof fo);
            break;
        }
        case BINDER_TYPE_FDA: {
            struct binder_fd_array_object fda;
            memcpy(&fda, obj, sizeof fda);
            uint64_t num_valid = (bo - off_start) / 8;
            struct binder_buffer_object parent;
            uint64_t parent_off;
            if (!validate_ptr(target_proc, b, fda.parent, off_start, num_valid, &parent, &parent_off)) {
                user_error("%d:%d got transaction with invalid parent offset or type", proc->pid, th->tid);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            if (!validate_fixup(target_proc, b, off_start, parent_off, fda.parent_offset,
                                last_fixup_obj_off, last_fixup_min_off)) {
                user_error("%d:%d got transaction with out-of-order buffer fixup", proc->pid, th->tid);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            uint64_t fd_buf = 4 * fda.num_fds;
            if (fda.num_fds >= (1ull << 60) || fd_buf > parent.length ||
                fda.parent_offset > parent.length - fd_buf) {
                user_error("%d:%d not enough space to store %lld fds in buffer",
                           proc->pid, th->tid, (long long)fda.num_fds);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            uint64_t fda_off = parent.buffer - user + fda.parent_offset;
            if (fda_off & 3) {
                user_error("%d:%d parent offset not aligned correctly", proc->pid, th->tid);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            for (uint64_t i = 0; i < fda.num_fds; i++) {
                int r = add_fixup(t, a, fda_off + 4 * i, fds_ok, th);
                if (r) { return_error = BR_FAILED_REPLY; param = r; goto err_translate; }
                uint32_t bad = 0xffffffffu;
                memcpy(base + fda_off + 4 * i, &bad, 4);
            }
            last_fixup_obj_off = parent_off;
            last_fixup_min_off = fda.parent_offset + 4 * fda.num_fds;
            break;
        }
        case BINDER_TYPE_PTR: {
            struct binder_buffer_object bp;
            memcpy(&bp, obj, sizeof bp);
            uint64_t left = sg_end - sg_off;
            if (bp.length > left) {
                user_error("%d:%d got transaction with too large buffer", proc->pid, th->tid);
                return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
            }
            if (sg_src + ALIGN8(bp.length) > a->sg_size || sg_src + bp.length < sg_src) {
                user_error("%d:%d got transaction with invalid offsets ptr", proc->pid, th->tid);
                return_error = BR_FAILED_REPLY; param = -L_EFAULT; goto err_translate;
            }
            memcpy(base + sg_off, a->sg + sg_src, bp.length);
            sg_src += ALIGN8(bp.length);
            bp.buffer = user + sg_off;
            sg_off += ALIGN8(bp.length);
            if (bp.flags & BINDER_BUFFER_FLAG_HAS_PARENT) {
                // binder_fixup_parent
                uint64_t num_valid = (bo - off_start) / 8;
                struct binder_buffer_object parent;
                uint64_t parent_off;
                if (!validate_ptr(target_proc, b, bp.parent, off_start, num_valid, &parent, &parent_off)) {
                    user_error("%d:%d got transaction with invalid parent offset or type", proc->pid, th->tid);
                    return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
                }
                if (!validate_fixup(target_proc, b, off_start, parent_off, bp.parent_offset,
                                    last_fixup_obj_off, last_fixup_min_off)) {
                    user_error("%d:%d got transaction with out-of-order buffer fixup", proc->pid, th->tid);
                    return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
                }
                if (parent.length < 8 || bp.parent_offset > parent.length - 8) {
                    user_error("%d:%d got transaction with invalid parent offset", proc->pid, th->tid);
                    return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
                }
                uint64_t at = bp.parent_offset + parent.buffer - user;
                if (at + 8 > b->size) {
                    return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
                }
                memcpy(base + at, &bp.buffer, 8);
            }
            memcpy(base + oo, &bp, sizeof bp);
            last_fixup_obj_off = oo;
            last_fixup_min_off = 0;
            break;
        }
        default:
            user_error("%d:%d got transaction with invalid object type, %x", proc->pid, th->tid, type);
            return_error = BR_FAILED_REPLY; param = -L_EINVAL; goto err_translate;
        }
    }

    t->work.type = W_TRANSACTION;
    debug("%d:%d %s %u -> pid %d: code %u flags 0x%x, %llu data, %llu offsets, %llu sg, %d fds",
          proc->pid, th->tid, reply ? "reply" : "transaction", t->debug_id, target_proc->pid,
          t->code, t->flags, (unsigned long long)tr->data_size,
          (unsigned long long)tr->offsets_size, (unsigned long long)extra_buffers_size, t->nfix);
    if (reply) {
        enqueue_thread_work(th, tcomplete);
        if (target_thread->is_dead) {
            return_error = BR_DEAD_REPLY; goto err_dead_proc;
        }
        pop_transaction(target_thread, in_reply_to);
        enqueue_thread_work(target_thread, &t->work);
        free_transaction(in_reply_to);
    } else if (!(t->flags & TF_ONE_WAY)) {
        t->need_reply = true;
        t->from_parent = th->transaction_stack;
        th->transaction_stack = t;
        enqueue_deferred_thread_work(th, tcomplete);
        if (!proc_transaction(t, target_proc, target_thread)) {
            th->transaction_stack = t->from_parent;   // binder_pop_transaction_ilocked
            return_error = BR_DEAD_REPLY;
            goto err_dead_proc;
        }
    } else {
        enqueue_thread_work(th, tcomplete);
        if (!proc_transaction(t, target_proc, NULL)) {
            return_error = BR_DEAD_REPLY;
            goto err_dead_proc;
        }
    }
    return;

err_dead_proc:
    // The transaction was complete but its target is gone: undo it whole.
    dequeue_work(tcomplete);
    free(tcomplete);
    tcomplete = NULL;
    bo = off_end;
    param = -L_ESRCH;
    why = "target died";
err_translate:
    buffer_release(target_proc, NULL, b, bo, true);
    target_node = NULL;                 // released with the buffer
    t->buffer = NULL;
    free_buf_mem(target_proc, b);
err_t:
    if (tcomplete) free(tcomplete);
    close_fixups(t);
    free(t);
err_node:
    if (target_node)
        dec_node(target_node, 1, 0);
err_early:
    if (*why)
        debug("%d:%d transaction failed %s/%d: %s", proc->pid, th->tid,
              return_error == BR_DEAD_REPLY ? "BR_DEAD_REPLY" : "BR_FAILED_REPLY", param, why);
    if (*why && return_error == BR_FAILED_REPLY)
        user_error("%d:%d transaction failed %d: %s", proc->pid, th->tid, param, why);
    if (in_reply_to) {
        th->return_error.cmd = BR_TRANSACTION_COMPLETE;
        enqueue_thread_work(th, &th->return_error.work);
        send_failed_reply(in_reply_to, return_error);
    } else {
        set_return_error(th, return_error);
    }
}

// binder_proc_transaction
static bool proc_transaction(struct transaction *t, struct proc *p, struct thread *th)
{
    struct node *node = t->buffer->target_node;
    bool oneway = (t->flags & TF_ONE_WAY) != 0;
    bool pending_async = false;
    if (oneway) {
        if (node->has_async_transaction) pending_async = true;
        else node->has_async_transaction = true;
    }
    if (p->is_dead || (th && th->is_dead)) {
        if (oneway && !pending_async) node->has_async_transaction = false;
        return false;
    }
    if (!th && !pending_async)
        th = select_thread(p);
    if (th)
        enqueue_thread_work(th, &t->work);
    else if (!pending_async)
        enqueue_work(&t->work, &p->todo);
    else
        enqueue_work(&t->work, &node->async_todo);
    return true;
}

// binder_free_buf (BC_FREE_BUFFER)
static void free_buf(struct proc *p, struct thread *th, struct buffer *b)
{
    if (b->transaction) {
        b->transaction->buffer = NULL;
        b->transaction = NULL;
    }
    if (b->async && b->target_node) {
        struct node *n = b->target_node;
        struct work *w = dequeue_head(&n->async_todo);
        if (!w)
            n->has_async_transaction = false;
        else
            enqueue_work(w, &p->todo);  // the next oneway call to this node
    }
    buffer_release(p, th, b, 0, false);
    if (b->clear_on_free && p->map)     // TF_CLEAR_BUF (Linux 5.11)
        memset(p->map + b->off, 0, b->size);
    free_buf_mem(p, b);
}

// ------------------------------------------------------------------ write

struct wr_ctx {
    const uint8_t *att;         // attachments cursor
    const uint8_t *att_end;
    int *fds;                   // descriptors of this request, -1 once taken
    int nfds;
    int fd_pos;
};

static bool next_att(struct wr_ctx *c, struct att *a)
{
    memset(a, 0, sizeof *a);
    if ((uint64_t)(c->att_end - c->att) < sizeof(struct bh_txn_att)) return false;
    struct bh_txn_att h;
    memcpy(&h, c->att, sizeof h);
    c->att += sizeof h;
    uint64_t need = ALIGN8(h.data_size) + ALIGN8(h.offsets_size) + h.sg_size;
    if (need < h.data_size || (uint64_t)(c->att_end - c->att) < need) return false;
    a->fault = h.fault;
    a->data = c->att;
    a->data_size = h.data_size;
    a->offsets = c->att + ALIGN8(h.data_size);
    a->offsets_size = h.offsets_size;
    a->sg = a->offsets + ALIGN8(h.offsets_size);
    a->sg_size = h.sg_size;
    c->att += need;
    if (h.nfds > (uint32_t)(c->nfds - c->fd_pos)) return false;
    a->fds = c->fds + c->fd_pos;
    a->nfds = (int)h.nfds;
    c->fd_pos += (int)h.nfds;
    return true;
}

// binder_thread_write. Returns 0 or -errno; *consumed advances per command.
static int thread_write(struct thread *th, const uint8_t *buf, uint64_t size,
                        uint64_t *consumed, struct wr_ctx *c)
{
    struct proc *proc = th->proc;
    uint64_t pos = 0;
    while (pos < size && th->return_error.cmd == BR_OK) {
        uint32_t cmd;
        if (size - pos < 4) return -L_EFAULT;
        memcpy(&cmd, buf + pos, 4);
        uint64_t argsz = LX_IOC_SIZE(cmd);
        if (size - pos - 4 < argsz) return -L_EFAULT;
        const uint8_t *arg = buf + pos + 4;
        switch (cmd) {
        case BC_INCREFS: case BC_ACQUIRE: case BC_RELEASE: case BC_DECREFS: {
            uint32_t target;
            memcpy(&target, arg, 4);
            bool strong = cmd == BC_ACQUIRE || cmd == BC_RELEASE;
            bool increment = cmd == BC_INCREFS || cmd == BC_ACQUIRE;
            int ret = -1;
            uint32_t desc = target;
            if (increment && !target) {
                struct node *cm = g_ctx_mgr[proc->context];
                if (cm) {
                    if (cm->proc == proc) {
                        user_error("%d:%d context manager tried to acquire desc 0", proc->pid, th->tid);
                        return -L_EINVAL;
                    }
                    ret = inc_ref_for_node(proc, cm, strong, NULL, &desc);
                }
            }
            if (ret)
                ret = update_ref_for_handle(proc, target, increment, strong);
            if (!ret && desc != target)
                user_error("%d:%d tried to acquire reference to desc %u, got %u instead",
                           proc->pid, th->tid, target, desc);
            if (ret)
                user_error("%d:%d refcount change on invalid ref %u ret %d", proc->pid, th->tid, target, ret);
            break;
        }
        case BC_INCREFS_DONE: case BC_ACQUIRE_DONE: {
            struct binder_ptr_cookie pc;
            memcpy(&pc, arg, sizeof pc);
            struct node *n = get_node(proc, pc.ptr);
            if (!n) {
                user_error("%d:%d %s u%016llx no match", proc->pid, th->tid,
                           cmd == BC_INCREFS_DONE ? "BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
                           (unsigned long long)pc.ptr);
                break;
            }
            if (pc.cookie != n->cookie) {
                user_error("%d:%d %s u%016llx node %u cookie mismatch", proc->pid, th->tid,
                           cmd == BC_INCREFS_DONE ? "BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
                           (unsigned long long)pc.ptr, n->debug_id);
                break;
            }
            if (cmd == BC_ACQUIRE_DONE) {
                if (!n->pending_strong_ref) {
                    user_error("%d:%d BC_ACQUIRE_DONE node %u has no pending acquire request",
                               proc->pid, th->tid, n->debug_id);
                    break;
                }
                n->pending_strong_ref = false;
            } else {
                if (!n->pending_weak_ref) {
                    user_error("%d:%d BC_INCREFS_DONE node %u has no pending increfs request",
                               proc->pid, th->tid, n->debug_id);
                    break;
                }
                n->pending_weak_ref = false;
            }
            if (dec_node_nilocked(n, cmd == BC_ACQUIRE_DONE, 0))
                free_node(n);       // the kernel WARNs here; it cannot happen
            break;
        }
        case BC_ATTEMPT_ACQUIRE:
        case BC_ACQUIRE_RESULT:
            hub_log("%d:%d BC_ATTEMPT_ACQUIRE/BC_ACQUIRE_RESULT not supported", proc->pid, th->tid);
            return -L_EINVAL;
        case BC_FREE_BUFFER: {
            uint64_t ptr;
            memcpy(&ptr, arg, 8);
            struct buffer *b = buffer_at_user(proc, ptr);
            if (!b) {
                user_error("%d:%d BC_FREE_BUFFER u%016llx no match", proc->pid, th->tid,
                           (unsigned long long)ptr);
                break;
            }
            if (!b->allow_user_free) {
                user_error("%d:%d BC_FREE_BUFFER u%016llx matched unreturned or currently freeing buffer",
                           proc->pid, th->tid, (unsigned long long)ptr);
                break;
            }
            free_buf(proc, th, b);
            break;
        }
        case BC_TRANSACTION: case BC_REPLY:
        case BC_TRANSACTION_SG: case BC_REPLY_SG: {
            struct binder_transaction_data tr;
            memcpy(&tr, arg, sizeof tr);
            uint64_t extra = 0;
            if (cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG)
                memcpy(&extra, arg + sizeof tr, 8);
            struct att a;
            if (!next_att(c, &a)) {
                hub_log("%d:%d transaction without its attachment", proc->pid, th->tid);
                return -L_EINVAL;
            }
            transaction(th, &tr, cmd == BC_REPLY || cmd == BC_REPLY_SG, extra, &a);
            for (int i = 0; i < a.nfds; i++)       // descriptors nothing took
                if (a.fds[i] >= 0) { close(a.fds[i]); a.fds[i] = -1; }
            break;
        }
        case BC_REGISTER_LOOPER:
            if (th->looper & LOOPER_ENTERED) {
                user_error("%d:%d ERROR: BC_REGISTER_LOOPER called after BC_ENTER_LOOPER", proc->pid, th->tid);
                th->looper |= LOOPER_INVALID;
            } else if (proc->requested_threads == 0) {
                user_error("%d:%d ERROR: BC_REGISTER_LOOPER called without request", proc->pid, th->tid);
                th->looper |= LOOPER_INVALID;
            } else {
                proc->requested_threads--;
                proc->requested_threads_started++;
            }
            th->looper |= LOOPER_REGISTERED;
            break;
        case BC_ENTER_LOOPER:
            if (th->looper & LOOPER_REGISTERED) {
                user_error("%d:%d ERROR: BC_ENTER_LOOPER called after BC_REGISTER_LOOPER", proc->pid, th->tid);
                th->looper |= LOOPER_INVALID;
            }
            th->looper |= LOOPER_ENTERED;
            break;
        case BC_EXIT_LOOPER:
            th->looper |= LOOPER_EXITED;
            break;
        case BC_REQUEST_DEATH_NOTIFICATION: case BC_CLEAR_DEATH_NOTIFICATION: {
            struct binder_handle_cookie hc;
            memcpy(&hc, arg, sizeof hc);
            struct ref *r = get_ref(proc, hc.handle, false);
            if (!r) {
                user_error("%d:%d %s invalid ref %u", proc->pid, th->tid,
                           cmd == BC_REQUEST_DEATH_NOTIFICATION ? "BC_REQUEST_DEATH_NOTIFICATION"
                                                                : "BC_CLEAR_DEATH_NOTIFICATION", hc.handle);
                break;
            }
            if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
                if (r->death) {
                    user_error("%d:%d BC_REQUEST_DEATH_NOTIFICATION death notification already set",
                               proc->pid, th->tid);
                    break;
                }
                struct ref_death *d = xcalloc(1, sizeof *d);
                LIST_INIT(&d->work.entry);
                d->cookie = hc.cookie;
                r->death = d;
                if (!r->node->proc) {
                    d->work.type = W_DEAD_BINDER;
                    enqueue_work(&d->work, &proc->todo);
                }
            } else {
                if (!r->death) {
                    user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification not active",
                               proc->pid, th->tid);
                    break;
                }
                struct ref_death *d = r->death;
                if (d->cookie != hc.cookie) {
                    user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification cookie mismatch %016llx != %016llx",
                               proc->pid, th->tid, (unsigned long long)d->cookie, (unsigned long long)hc.cookie);
                    break;
                }
                r->death = NULL;
                if (!work_queued(&d->work)) {
                    d->work.type = W_CLEAR_DEATH_NOTIFICATION;
                    if (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED))
                        enqueue_thread_work(th, &d->work);
                    else
                        enqueue_work(&d->work, &proc->todo);
                } else {
                    d->work.type = W_DEAD_BINDER_AND_CLEAR;
                }
            }
            break;
        }
        case BC_DEAD_BINDER_DONE: {
            uint64_t cookie;
            memcpy(&cookie, arg, 8);
            struct ref_death *d = NULL;
            struct list *e;
            for (e = proc->delivered_death.next; e != &proc->delivered_death; e = e->next) {
                struct ref_death *x = container_of(container_of(e, struct work, entry), struct ref_death, work);
                if (x->cookie == cookie) { d = x; break; }
            }
            if (!d) {
                user_error("%d:%d BC_DEAD_BINDER_DONE %016llx not found", proc->pid, th->tid,
                           (unsigned long long)cookie);
                break;
            }
            dequeue_work(&d->work);
            if (d->work.type == W_DEAD_BINDER_AND_CLEAR) {
                d->work.type = W_CLEAR_DEATH_NOTIFICATION;
                if (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED))
                    enqueue_thread_work(th, &d->work);
                else
                    enqueue_work(&d->work, &proc->todo);
            }
            // W_DEAD_BINDER: the ref was cleared or never will be; the
            // notification is done and stays owned by its ref.
            break;
        }
        default:
            hub_log("%d:%d unknown command %u (0x%x)", proc->pid, th->tid, cmd, cmd);
            return -L_EINVAL;
        }
        pos += 4 + argsz;
        *consumed = pos;
    }
    return 0;
}

// ------------------------------------------------------------------ read

struct rd {
    uint8_t *out;
    uint64_t avail, pos;
    uint64_t consumed0;         // read_consumed when the call started
    bool spawn;
    int *fds; uint64_t *fix; int nfix;
    int64_t ret;
};

static void put32(struct rd *r, uint32_t v) { memcpy(r->out + r->pos, &v, 4); r->pos += 4; }
static void put64(struct rd *r, uint64_t v) { memcpy(r->out + r->pos, &v, 8); r->pos += 8; }

enum { RD_DONE, RD_BLOCK };

// binder_thread_read. RD_BLOCK: nothing to return yet (park the thread).
static int thread_read(struct thread *th, struct rd *r, bool nonblock)
{
    struct proc *proc = th->proc;
retry:
    r->pos = 0;
    bool wfpw = available_for_proc_work(th);
    if (wfpw && !(th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED)))
        user_error("%d:%d ERROR: Thread waiting for process work before calling BC_REGISTER_LOOPER or BC_ENTER_LOOPER (state %x)",
                   proc->pid, th->tid, th->looper);
    if (!has_work(th, wfpw)) {
        if (nonblock) { r->ret = -L_EAGAIN; r->pos = 0; return RD_DONE; }
        return RD_BLOCK;
    }
    if (r->consumed0 == 0) {
        if (r->avail < 4) { r->ret = 0; return RD_DONE; }
        put32(r, BR_NOOP);
    }
    while (1) {
        struct list *list;
        if (!list_empty(&th->todo)) list = &th->todo;
        else if (!list_empty(&proc->todo) && wfpw) list = &proc->todo;
        else {
            if (r->consumed0 + r->pos == 4 && !th->looper_need_return)
                goto retry;
            break;
        }
        if (r->avail - r->pos < sizeof(struct binder_transaction_data_secctx) + 4)
            break;
        struct work *w = dequeue_head(list);
        if (list_empty(&th->todo)) th->process_todo = false;
        struct transaction *t = NULL;
        switch (w->type) {
        case W_TRANSACTION:
            t = container_of(w, struct transaction, work);
            break;
        case W_RETURN_ERROR: {
            struct error_work *e = container_of(w, struct error_work, work);
            put32(r, e->cmd);
            e->cmd = BR_OK;
            break;
        }
        case W_TRANSACTION_COMPLETE:
            free(w);
            put32(r, BR_TRANSACTION_COMPLETE);
            break;
        case W_NODE: {
            struct node *n = container_of(w, struct node, work);
            bool strong = n->internal_strong_refs || n->local_strong_refs;
            bool weak = !list_empty(&n->refs) || n->local_weak_refs || strong;
            bool has_weak = n->has_weak_ref, has_strong = n->has_strong_ref;
            uint64_t ptr = n->ptr, cookie = n->cookie;
            if (weak && !has_weak) { n->has_weak_ref = true; n->pending_weak_ref = true; n->local_weak_refs++; }
            if (strong && !has_strong) { n->has_strong_ref = true; n->pending_strong_ref = true; n->local_strong_refs++; }
            if (!strong && has_strong) n->has_strong_ref = false;
            if (!weak && has_weak) n->has_weak_ref = false;
            if (!weak && !strong) {
                debug("%d:%d node %u u%016llx deleted", proc->pid, th->tid, n->debug_id, (unsigned long long)ptr);
                free_node(n);
            }
            if (weak && !has_weak) { put32(r, BR_INCREFS); put64(r, ptr); put64(r, cookie); }
            if (strong && !has_strong) { put32(r, BR_ACQUIRE); put64(r, ptr); put64(r, cookie); }
            if (!strong && has_strong) { put32(r, BR_RELEASE); put64(r, ptr); put64(r, cookie); }
            if (!weak && has_weak) { put32(r, BR_DECREFS); put64(r, ptr); put64(r, cookie); }
            break;
        }
        case W_DEAD_BINDER:
        case W_DEAD_BINDER_AND_CLEAR:
        case W_CLEAR_DEATH_NOTIFICATION: {
            struct ref_death *d = container_of(w, struct ref_death, work);
            uint32_t cmd = w->type == W_CLEAR_DEATH_NOTIFICATION ? BR_CLEAR_DEATH_NOTIFICATION_DONE
                                                                 : BR_DEAD_BINDER;
            uint64_t cookie = d->cookie;
            if (w->type == W_CLEAR_DEATH_NOTIFICATION)
                free(d);
            else
                enqueue_work(w, &proc->delivered_death);
            put32(r, cmd);
            put64(r, cookie);
            if (cmd == BR_DEAD_BINDER)
                goto done;      // DEAD_BINDER notifications can cause transactions
            break;
        }
        default:
            hub_log("%d:%d bad work type %d", proc->pid, th->tid, w->type);
            break;
        }
        if (!t)
            continue;

        struct binder_transaction_data_secctx tr;
        memset(&tr, 0, sizeof tr);
        struct binder_transaction_data *trd = &tr.transaction_data;
        uint32_t cmd;
        if (!t->buffer) {
            // Its buffer went with a dying thread; nothing left to deliver.
            free_transaction(t);
            continue;
        }
        if (t->buffer->target_node) {
            trd->target.ptr = t->buffer->target_node->ptr;
            trd->cookie = t->buffer->target_node->cookie;
            cmd = BR_TRANSACTION;
        } else {
            cmd = BR_REPLY;
        }
        trd->code = t->code;
        trd->flags = t->flags;
        trd->sender_euid = t->sender_euid;
        trd->sender_pid = t->from ? t->from->proc->pid : 0;
        // binder_apply_fd_fixups: the descriptors go with this result and the
        // receiver's runtime installs them (it knows its own fd numbers).
        for (int i = 0; i < t->nfix; i++) {
            r->fds[r->nfix] = t->fix_fd[i];
            r->fix[r->nfix] = t->buffer->off + t->fix_off[i];
            r->nfix++;
            t->fix_fd[i] = -1;
        }
        close_fixups(t);
        trd->data_size = t->buffer->data_size;
        trd->offsets_size = t->buffer->offsets_size;
        trd->data.ptr.buffer = proc->user_base + t->buffer->off;
        trd->data.ptr.offsets = trd->data.ptr.buffer + ALIGN8(t->buffer->data_size);
        size_t trsize = sizeof *trd;
        tr.secctx = t->security_ctx;
        if (t->security_ctx) {
            cmd = BR_TRANSACTION_SEC_CTX;
            trsize = sizeof tr;
        }
        put32(r, cmd);
        memcpy(r->out + r->pos, &tr, trsize);
        r->pos += trsize;
        t->buffer->allow_user_free = true;
        if (cmd != BR_REPLY && !(t->flags & TF_ONE_WAY)) {
            t->to_parent = th->transaction_stack;
            t->to_thread = th;
            th->transaction_stack = t;
        } else {
            free_transaction(t);
        }
        break;
    }
done:
    if (proc->requested_threads == 0 && list_empty(&proc->waiting_threads) &&
        proc->requested_threads_started < proc->max_threads &&
        (th->looper & (LOOPER_REGISTERED | LOOPER_ENTERED))) {
        proc->requested_threads++;
        r->spawn = true;
        debug("%d:%d BR_SPAWN_LOOPER", proc->pid, th->tid);
    }
    r->ret = 0;
    return RD_DONE;
}

// ------------------------------------------------------------------ release

// binder_thread_release
static void thread_release(struct thread *th)
{
    struct proc *p = th->proc;
    list_del_init(&th->proc_entry);
    list_del_init(&th->waiting_entry);
    th->parked = false;
    struct transaction *t = th->transaction_stack, *send_reply = NULL;
    if (t && t->to_thread == th) send_reply = t;
    th->is_dead = true;
    while (t) {
        if (t->to_thread == th) {
            t->to_proc = NULL;
            t->to_thread = NULL;
            if (t->buffer) {
                t->buffer->transaction = NULL;
                t->buffer = NULL;
            }
            t = t->to_parent;
        } else if (t->from == th) {
            t->from = NULL;
            t = t->from_parent;
        } else {
            hub_log("%d:%d thread_release: bad transaction stack", p->pid, th->tid);
            break;
        }
    }
    if (send_reply)
        send_failed_reply(send_reply, BR_DEAD_REPLY);
    release_work(&th->todo);
    // FDA descriptors still owed to a gone thread stay open in its process
    // (they were installed there); nothing here can close them.
    free(th->closes);
    for (int i = 0; i < th->nheld; i++) close(th->held[i]);
    free(th->held);
    if (th->fd >= 0) close(th->fd);
    debug("%d:%d thread released", p->pid, th->tid);
    free(th);
}

static void node_release(struct node *n)
{
    struct proc *p = n->proc;
    release_work(&n->async_todo);
    dequeue_work(&n->work);
    list_del_init(&n->proc_entry);
    if (list_empty(&n->refs)) {
        free(n);
        return;
    }
    n->proc = NULL;
    n->local_strong_refs = 0;
    n->local_weak_refs = 0;
    list_add_tail(&n->proc_entry, &g_dead_nodes);
    struct list *e;
    for (e = n->refs.next; e != &n->refs; e = e->next) {
        struct ref *r = container_of(e, struct ref, node_entry);
        if (!r->death) continue;
        if (work_queued(&r->death->work)) {
            hub_log("node_release: death work already queued");
            continue;
        }
        r->death->work.type = W_DEAD_BINDER;
        enqueue_work(&r->death->work, &r->proc->todo);
    }
    debug("%d node %u now dead", p ? p->pid : 0, n->debug_id);
}

static const char *g_read_fail;     // read_msg's last failure (below)

// binder_deferred_release
static void proc_release(struct proc *p)
{
    if (p->hello)
        hub_log("proc %u (pid %d) released%s%s", p->id, p->pid, g_read_fail[0] ? ": " : "", g_read_fail);
    list_del_init(&p->entry);
    if (g_ctx_mgr[p->context] && g_ctx_mgr[p->context]->proc == p)
        g_ctx_mgr[p->context] = NULL;
    p->is_dead = true;
    while (!list_empty(&p->threads))
        thread_release(container_of(p->threads.next, struct thread, proc_entry));
    while (!list_empty(&p->nodes))
        node_release(container_of(p->nodes.next, struct node, proc_entry));
    while (!list_empty(&p->refs))
        cleanup_ref(container_of(p->refs.next, struct ref, proc_entry));
    release_work(&p->todo);
    release_work(&p->delivered_death);
    while (!list_empty(&p->buffers)) {
        struct buffer *b = container_of(p->buffers.next, struct buffer, entry);
        if (b->transaction) {
            b->transaction->buffer = NULL;
        }
        free_buf_mem(p, b);
    }
    if (p->map) munmap(p->map, p->map_host_size);
    if (p->fd >= 0) close(p->fd);
    debug("proc %d (pid %d) released", p->id, p->pid);
    free(p);
}

// ------------------------------------------------------------------ channels

static int send_all(int fd, const struct iovec *iov0, int niov, const int *fds, int nfds)
{
    struct iovec iov[8];
    if (niov > 8) return -1;
    memcpy(iov, iov0, sizeof(struct iovec) * (size_t)niov);
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; } cm;
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = niov };
    if (nfds > 0) {
        m.msg_control = cm.b;
        m.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    int idx = 0;
    while (idx < niov) {
        m.msg_iov = iov + idx;
        m.msg_iovlen = niov - idx;
        ssize_t n = sendmsg(fd, &m, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        m.msg_control = NULL;
        m.msg_controllen = 0;
        while (n > 0 && idx < niov) {
            if ((size_t)n >= iov[idx].iov_len) { n -= (ssize_t)iov[idx].iov_len; idx++; }
            else { iov[idx].iov_base = (char *)iov[idx].iov_base + n; iov[idx].iov_len -= (size_t)n; n = 0; }
        }
        while (idx < niov && iov[idx].iov_len == 0) idx++;
    }
    return 0;
}

// Why the last read_msg failed, for the log line of whoever drops the peer.
static const char *g_read_fail = "";
static int g_read_errno;

static int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_read_fail = n == 0 ? "end of file in the payload" : "payload read failed";
            g_read_errno = n == 0 ? 0 : errno;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

// One whole message. -1: the peer is gone (or broke the protocol).
static int read_msg(int fd, struct bh_hdr *h, uint8_t **payload, int *fds, int *nfds)
{
    *nfds = 0;
    *payload = NULL;
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; } cm;
    struct iovec iov = { h, sizeof *h };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.b,
                        .msg_controllen = sizeof cm.b };
    size_t got = 0;
    while (got < sizeof *h) {
        iov.iov_base = (char *)h + got;
        iov.iov_len = sizeof *h - got;
        m.msg_control = cm.b;
        m.msg_controllen = sizeof cm.b;
        ssize_t n = recvmsg(fd, &m, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            g_read_fail = n == 0 ? (got ? "end of file in the header" : "end of file") : "header read failed";
            g_read_errno = n == 0 ? 0 : errno;
            goto fail;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < k; i++) {
                    int f;
                    memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
                    if (*nfds < BH_MAX_FDS) { fcntl(f, F_SETFD, FD_CLOEXEC); fds[(*nfds)++] = f; }
                    else close(f);
                }
            }
        }
        got += (size_t)n;
    }
    if (h->len > BH_MAX_MSG) { g_read_fail = "message too large"; g_read_errno = 0; goto fail; }
    if (h->len) {
        *payload = malloc(h->len);
        if (!*payload) { g_read_fail = "out of memory"; g_read_errno = ENOMEM; goto fail; }
        if (read_full(fd, *payload, h->len) != 0) goto fail;
    }
    if ((int)h->nfds != *nfds) {
        hub_log("message carried %d descriptors, header says %u", *nfds, h->nfds);
        goto fail;
    }
    return 0;
fail:
    free(*payload);
    *payload = NULL;
    for (int i = 0; i < *nfds; i++) close(fds[i]);
    *nfds = 0;
    return -1;
}

static bool proc_poll_wanted(struct proc *p)
{
    struct list *e;
    for (e = p->threads.next; e != &p->threads; e = e->next) {
        struct thread *t = container_of(e, struct thread, proc_entry);
        if (!(t->looper & LOOPER_POLL) || t->parked) continue;
        if (has_work(t, available_for_proc_work(t))) return true;
    }
    return false;
}

static void proc_ring(struct proc *p)
{
    if (!p->hello || p->fd < 0) return;
    if (proc_poll_wanted(p) && p->bells == p->bells_drained) {
        char c = 'D';
        if (send(p->fd, &c, 1, MSG_DONTWAIT) == 1)
            p->bells++;
    }
}

static void send_result(struct thread *th, uint64_t seq, struct bh_result *res,
                        const uint8_t *rd, const int *fds, const uint64_t *fix)
{
    struct proc *p = th->proc;
    res->ncloses = (uint32_t)th->ncloses;
    // The doorbell's state as of this result: rung before it, if wanted.
    proc_ring(p);
    res->bells = p->bells;
    res->bell_pending = proc_poll_wanted(p);
    if (!res->bell_pending)
        p->bells_drained = p->bells;
    struct bh_hdr h = { .type = BH_RESULT, .nfds = res->nfixups, .seq = seq };
    h.len = sizeof *res + res->read_len + 8ull * res->nfixups + 4ull * res->ncloses;
    struct iovec iov[5] = {
        { &h, sizeof h }, { res, sizeof *res },
        { (void *)rd, res->read_len }, { (void *)fix, 8ull * res->nfixups },
        { th->closes, 4ull * (size_t)th->ncloses },
    };
    if (send_all(th->fd, iov, 5, fds, (int)res->nfixups) != 0)
        hub_log("%d:%d result not delivered: %s", p->pid, th->tid, strerror(errno));
    // Not closed yet: a descriptor whose only reference is the message in
    // flight can be flushed by XNU's unix socket garbage collection before
    // the thread reads it (see runtime/binder.c tchan_get). The thread's
    // next message proves it has them (held_release).
    for (uint32_t i = 0; i < res->nfixups; i++) {
        if (th->nheld == th->cheld) {
            th->cheld = th->cheld ? th->cheld * 2 : 8;
            th->held = realloc(th->held, (size_t)th->cheld * sizeof(int));
            if (!th->held) { hub_log("out of memory"); _exit(70); }
        }
        th->held[th->nheld++] = fds[i];
    }
    th->ncloses = 0;
}

// Run the parked read of `th`; send its result when it has one.
static void try_complete_read(struct thread *th)
{
    if (!th->parked) return;
    list_del_init(&th->waiting_entry);
    uint64_t avail = th->park_read_avail;
    struct rd r = { .avail = avail, .consumed0 = th->park_read_consumed };
    r.out = malloc(avail + 256);        // slack: node commands are not bounds-checked
    int fds[BH_MAX_FDS];
    uint64_t fix[BH_MAX_FDS];
    r.fds = fds; r.fix = fix;
    if (!r.out) { hub_log("out of memory"); _exit(70); }
    int st = thread_read(th, &r, th->park_nonblock);
    if (st == RD_BLOCK) {
        if (available_for_proc_work(th))
            list_add_tail(&th->waiting_entry, &th->proc->waiting_threads);
        free(r.out);
        return;
    }
    th->parked = false;
    th->looper &= ~LOOPER_WAITING;
    th->looper_need_return = false;       // end of binder_ioctl
    struct bh_result res = { .ret = r.ret, .write_consumed = th->park_write_consumed,
                             .read_len = r.ret == 0 ? (r.pos > avail ? avail : r.pos) : 0,
                             .spawn_looper = r.spawn,
                             .nfixups = (uint32_t)r.nfix };
    send_result(th, th->park_seq, &res, r.out, fds, fix);
    free(r.out);
}

static void simple_result(struct thread *th, uint64_t seq, int64_t ret, const void *arg, size_t argn)
{
    struct bh_result res = { .ret = ret };
    if (arg && argn <= sizeof res.arg) memcpy(res.arg, arg, argn);
    th->looper_need_return = false;
    send_result(th, seq, &res, NULL, NULL, NULL);
}

static void handle_write_read(struct thread *th, const struct bh_hdr *h, const uint8_t *pl,
                              int *fds, int nfds)
{
    struct bh_wr wr;
    if (h->len < sizeof wr) { simple_result(th, h->seq, -L_EINVAL, NULL, 0); return; }
    memcpy(&wr, pl, sizeof wr);
    const uint8_t *wbuf = pl + sizeof wr;
    uint64_t rest = h->len - sizeof wr;
    if (wr.write_len > rest) { simple_result(th, h->seq, -L_EINVAL, NULL, 0); return; }
    struct wr_ctx c = { .att = wbuf + ALIGN8(wr.write_len), .att_end = pl + h->len,
                        .fds = fds, .nfds = nfds };
    if (ALIGN8(wr.write_len) > rest) c.att = c.att_end;
    uint64_t consumed = 0;
    int ret = 0;
    if (wr.write_len > 0)
        ret = thread_write(th, wbuf, wr.write_len, &consumed, &c);
    for (int i = 0; i < nfds; i++)      // descriptors of transactions not reached
        if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
    if (ret < 0) {
        struct bh_result res = { .ret = ret, .write_consumed = consumed };
        res.arg[0] = 1;                 // the write failed: read_consumed = 0
        th->looper_need_return = false;
        send_result(th, h->seq, &res, NULL, NULL, NULL);
        return;
    }
    if (wr.read_avail == 0) {
        struct bh_result res = { .ret = 0, .write_consumed = consumed };
        th->looper_need_return = false;
        send_result(th, h->seq, &res, NULL, NULL, NULL);
        return;
    }
    th->parked = true;
    th->looper |= LOOPER_WAITING;
    th->park_seq = h->seq;
    th->park_write_consumed = consumed;
    th->park_read_avail = wr.read_avail > (1u << 20) ? (1u << 20) : wr.read_avail;
    th->park_read_consumed = wr.read_consumed;
    th->park_nonblock = (wr.flags & BH_WR_NONBLOCK) != 0;
    try_complete_read(th);
}

static void handle_ioctl(struct thread *th, const struct bh_hdr *h, const uint8_t *pl)
{
    struct proc *p = th->proc;
    struct bh_ioctl io;
    if (h->len < sizeof io) { simple_result(th, h->seq, -L_EINVAL, NULL, 0); return; }
    memcpy(&io, pl, sizeof io);
    switch (io.cmd) {
    case BINDER_VERSION: {
        struct binder_version v = { BINDER_CURRENT_PROTOCOL_VERSION };
        simple_result(th, h->seq, 0, &v, sizeof v);
        return;
    }
    case BINDER_SET_MAX_THREADS: {
        uint32_t n;
        memcpy(&n, io.arg, 4);
        p->max_threads = (int)n;
        simple_result(th, h->seq, 0, NULL, 0);
        return;
    }
    case BINDER_SET_CONTEXT_MGR_EXT:
    case BINDER_SET_CONTEXT_MGR: {
        struct flat_binder_object fbo;
        memset(&fbo, 0, sizeof fbo);
        if (io.cmd == BINDER_SET_CONTEXT_MGR_EXT)
            memcpy(&fbo, io.arg, sizeof fbo);
        if (g_ctx_mgr[p->context]) {
            hub_log("BINDER_SET_CONTEXT_MGR already set");
            simple_result(th, h->seq, -L_EBUSY, NULL, 0);
            return;
        }
        if (g_ctx_mgr_uid_set[p->context]) {
            if (g_ctx_mgr_uid[p->context] != p->euid) {
                hub_log("BINDER_SET_CONTEXT_MGR bad uid %u != %u", p->euid, g_ctx_mgr_uid[p->context]);
                simple_result(th, h->seq, -L_EPERM, NULL, 0);
                return;
            }
        } else {
            g_ctx_mgr_uid[p->context] = p->euid;
            g_ctx_mgr_uid_set[p->context] = true;
        }
        struct node *n = new_node(p, &fbo);
        n->local_weak_refs++;
        n->local_strong_refs++;
        n->has_strong_ref = true;
        n->has_weak_ref = true;
        g_ctx_mgr[p->context] = n;
        debug("%d context manager of context %d", p->pid, p->context);
        simple_result(th, h->seq, 0, NULL, 0);
        return;
    }
    case BINDER_THREAD_EXIT:
        // The runtime closes the channel after this result; the channel's
        // end is where the thread is released.
        simple_result(th, h->seq, 0, NULL, 0);
        return;
    case BH_IOCTL_UNMAPPED:
        p->unmapped = true;
        simple_result(th, h->seq, 0, NULL, 0);
        return;
    case BINDER_GET_NODE_DEBUG_INFO: {
        struct binder_node_debug_info info;
        memcpy(&info, io.arg, sizeof info);
        uint64_t want = info.ptr;
        memset(&info, 0, sizeof info);
        struct node *best = NULL;
        struct list *e;
        for (e = p->nodes.next; e != &p->nodes; e = e->next) {
            struct node *n = container_of(e, struct node, proc_entry);
            if (n->ptr > want && (!best || n->ptr < best->ptr)) best = n;
        }
        if (best) {
            info.ptr = best->ptr;
            info.cookie = best->cookie;
            info.has_strong_ref = best->has_strong_ref;
            info.has_weak_ref = best->has_weak_ref;
        }
        simple_result(th, h->seq, 0, &info, sizeof info);
        return;
    }
    case BINDER_GET_NODE_INFO_FOR_REF: {
        struct binder_node_info_for_ref info;
        memcpy(&info, io.arg, sizeof info);
        if (info.strong_count || info.weak_count || info.reserved1 || info.reserved2 || info.reserved3) {
            simple_result(th, h->seq, -L_EINVAL, NULL, 0);
            return;
        }
        struct node *cm = g_ctx_mgr[p->context];
        if (!cm || cm->proc != p) {
            simple_result(th, h->seq, -L_EPERM, NULL, 0);
            return;
        }
        struct ref *r = get_ref(p, info.handle, true);
        if (!r) {
            simple_result(th, h->seq, -L_EINVAL, NULL, 0);
            return;
        }
        info.strong_count = (uint32_t)(r->node->local_strong_refs + r->node->internal_strong_refs);
        info.weak_count = (uint32_t)r->node->local_weak_refs;
        simple_result(th, h->seq, 0, &info, sizeof info);
        return;
    }
    default:
        simple_result(th, h->seq, -L_EINVAL, NULL, 0);
        return;
    }
}

static void handle_mmap(struct thread *th, const struct bh_hdr *h, const uint8_t *pl,
                        int *fds, int nfds)
{
    struct proc *p = th->proc;
    struct bh_mmap mm;
    if (h->len < sizeof mm || nfds != 1) {
        for (int i = 0; i < nfds; i++) close(fds[i]);
        simple_result(th, h->seq, -L_EINVAL, NULL, 0);
        return;
    }
    memcpy(&mm, pl, sizeof mm);
    int fd = fds[0];
    fds[0] = -1;
    if (p->map) {
        close(fd);
        hub_log("%d binder_mmap: already mapped", p->pid);
        simple_result(th, h->seq, -L_EBUSY, NULL, 0);
        return;
    }
    uint64_t size = mm.size > SZ_4M ? SZ_4M : mm.size;
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t hsize = (size + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
    void *m = mmap(NULL, hsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        hub_log("%d binder_mmap: %s", p->pid, strerror(errno));
        simple_result(th, h->seq, -L_ENOMEM, NULL, 0);
        return;
    }
    p->map = m;
    p->map_size = size;
    p->map_host_size = hsize;
    p->user_base = mm.base;
    p->free_async = (int64_t)(size / 2);
    debug("%d mapped %llu bytes at guest 0x%llx", p->pid, (unsigned long long)size,
          (unsigned long long)mm.base);
    simple_result(th, h->seq, 0, NULL, 0);
}

// ------------------------------------------------------------------ loop

static struct thread *new_thread(struct proc *p, int fd, int tid)
{
    struct thread *t = xcalloc(1, sizeof *t);
    t->proc = p;
    t->fd = fd;
    t->tid = tid;
    t->looper_need_return = true;
    LIST_INIT(&t->todo);
    LIST_INIT(&t->waiting_entry);
    t->return_error.work.type = W_RETURN_ERROR;
    t->return_error.cmd = BR_OK;
    LIST_INIT(&t->return_error.work.entry);
    t->reply_error.work.type = W_RETURN_ERROR;
    t->reply_error.cmd = BR_OK;
    LIST_INIT(&t->reply_error.work.entry);
    list_add_tail(&t->proc_entry, &p->threads);
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int sz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    debug("%d:%d new thread", p->pid, tid);
    return t;
}

static void proc_message(struct proc *p)
{
    struct bh_hdr h;
    uint8_t *pl;
    int fds[BH_MAX_FDS], nfds;
    if (read_msg(p->fd, &h, &pl, fds, &nfds) != 0) {
        proc_release(p);
        return;
    }
    if (!p->hello) {
        struct bh_hello hello;
        struct bh_hello_ack ack = { 0, 0 };
        if (h.type != BH_HELLO || h.len < sizeof hello) {
            free(pl);
            for (int i = 0; i < nfds; i++) close(fds[i]);
            proc_release(p);
            return;
        }
        memcpy(&hello, pl, sizeof hello);
        free(pl);
        for (int i = 0; i < nfds; i++) close(fds[i]);
        if (hello.version != BH_VERSION || hello.context > 2) {
            ack.ret = -L_EINVAL;
        } else {
            p->hello = true;
            p->pid = hello.pid;
            pid_t hp = 0;
            socklen_t hplen = sizeof hp;
            p->host_pid = getsockopt(p->fd, SOL_LOCAL, LOCAL_PEERPID,
                                     &hp, &hplen) == 0 ? hp : hello.pid;
            p->euid = hello.euid;
            p->context = (int)hello.context;
            memcpy(p->secctx, hello.secctx, sizeof p->secctx);
            p->secctx[sizeof p->secctx - 1] = 0;
            ack.proc_id = p->id;
            debug("proc %u: pid %d euid %u context %d", p->id, p->pid, p->euid, p->context);
        }
        struct bh_hdr ah = { .type = BH_HELLO_ACK, .len = sizeof ack, .seq = h.seq };
        struct iovec iov[2] = { { &ah, sizeof ah }, { &ack, sizeof ack } };
        if (send_all(p->fd, iov, 2, NULL, 0) != 0 || ack.ret) {
            proc_release(p);
        }
        return;
    }
    if (h.type == BH_THREAD && nfds == 1 && h.len >= sizeof(struct bh_thread)) {
        struct bh_thread bt;
        memcpy(&bt, pl, sizeof bt);
        char c;
        ssize_t pk = recv(fds[0], &c, 1, MSG_PEEK | MSG_DONTWAIT);
        int pe = errno;
        struct stat fst;
        bool sock = fstat(fds[0], &fst) == 0 && S_ISSOCK(fst.st_mode);
        pid_t peer = 0;
        socklen_t pl2 = sizeof peer;
        int gp = getsockopt(fds[0], SOL_LOCAL, LOCAL_PEERPID, &peer, &pl2);
        if (pk == 0 || !sock || (gp == 0 && peer != p->host_pid)) {
            // The runtime waits for BH_THREAD_ACK and makes a new channel
            // when it does not come (runtime/binder.c tchan_get).
            hub_log("%d:%d thread channel arrived %s (socket %d, peer pid %d%s); refused", p->pid, bt.tid,
                    pk == 0 ? "closed at the other end" : "odd", sock, gp == 0 ? peer : -1,
                    pk < 0 && pe != EAGAIN ? ", peek failed" : "");
            close(fds[0]);
        } else {
            struct thread *nt = new_thread(p, fds[0], bt.tid);
            struct bh_hdr ack = { .type = BH_THREAD_ACK };
            struct iovec iov = { &ack, sizeof ack };
            if (send_all(nt->fd, &iov, 1, NULL, 0) != 0) {
                hub_log("%d:%d thread channel: acknowledgement not sent: %s; dropped", p->pid, bt.tid, strerror(errno));
                thread_release(nt);
            }
        }
    } else {
        hub_log("proc %d: unexpected message %u", p->pid, h.type);
        for (int i = 0; i < nfds; i++) close(fds[i]);
    }
    free(pl);
}

static void held_release(struct thread *th)
{
    for (int i = 0; i < th->nheld; i++) close(th->held[i]);
    th->nheld = 0;
}

static void thread_message(struct thread *th)
{
    struct bh_hdr h;
    uint8_t *pl;
    int fds[BH_MAX_FDS], nfds;
    if (read_msg(th->fd, &h, &pl, fds, &nfds) != 0) {
        // A thread's channel ends when its thread or process does (end of
        // file, logged only when it was in the middle of something); any
        // other failure drops a live thread, whose next call then fails with
        // EBADF ("Bad file descriptor" in Java): always logged.
        if (strcmp(g_read_fail, "end of file") != 0 || th->parked || th->transaction_stack)
            hub_log("%d:%d thread dropped: %s%s%s%s", th->proc->pid, th->tid, g_read_fail,
                    g_read_errno ? " (" : "", g_read_errno ? strerror(g_read_errno) : "", g_read_errno ? ")" : "");
        thread_release(th);
        return;
    }
    // A request (not a cancel, which can cross the result) is sent after
    // the last result was read: the descriptors that came with it are the
    // thread's now.
    if (h.type != BH_CANCEL)
        held_release(th);
    if (th->parked && h.type != BH_CANCEL) {
        hub_log("%d:%d request while blocked; dropping the thread", th->proc->pid, th->tid);
        for (int i = 0; i < nfds; i++) close(fds[i]);
        free(pl);
        thread_release(th);
        return;
    }
    switch (h.type) {
    case BH_WRITE_READ:
        handle_write_read(th, &h, pl, fds, nfds);
        break;
    case BH_CANCEL:
        if (th->parked && th->park_seq == h.seq) {
            list_del_init(&th->waiting_entry);
            th->parked = false;
            th->looper &= ~LOOPER_WAITING;
            struct bh_result res = { .ret = -L_EINTR, .write_consumed = th->park_write_consumed };
            send_result(th, h.seq, &res, NULL, NULL, NULL);
        }
        break;
    case BH_IOCTL:
        handle_ioctl(th, &h, pl);
        break;
    case BH_MMAP:
        handle_mmap(th, &h, pl, fds, nfds);
        nfds = 0;
        break;
    case BH_POLL:
        th->looper |= LOOPER_POLL;
        {
            struct bh_result res = { .ret = 0 };
            send_result(th, h.seq, &res, NULL, NULL, NULL);
        }
        break;
    default:
        hub_log("%d:%d unknown message %u", th->proc->pid, th->tid, h.type);
        break;
    }
    if (h.type != BH_WRITE_READ && h.type != BH_MMAP)
        for (int i = 0; i < nfds; i++) close(fds[i]);
    free(pl);
}

// After every event: parked threads with work get their result, and every
// process's doorbell follows its state.
static void settle(void)
{
    struct list *pe, *pt;
    list_for_each_safe(pe, pt, &g_procs) {
        struct proc *p = container_of(pe, struct proc, entry);
        struct list *te, *tt;
        list_for_each_safe(te, tt, &p->threads) {
            struct thread *th = container_of(te, struct thread, proc_entry);
            if (th->parked && has_work(th, available_for_proc_work(th)))
                try_complete_read(th);
        }
        // Proc work nobody is waiting for: give it to a parked looper that
        // can now take it (a thread whose stack emptied), as binder_wakeup_proc.
        proc_ring(p);
    }
}

static volatile sig_atomic_t g_stop;
static void on_term(int s) { (void)s; g_stop = 1; }

int lxrt_binder_hub_main(int argc, char **argv)
{
    // argv: <lxrun> --binder-hub <dir> [--daemon]
    if (argc < 3) {
        fprintf(stderr, "usage: lxrun --binder-hub <dir> [--daemon]\n");
        return 2;
    }
    g_dir = argv[2];
    bool daemonize = argc > 3 && !strcmp(argv[3], "--daemon");
    g_verbose = getenv("LXRT_BINDER_LOG") && *getenv("LXRT_BINDER_LOG") == '1';
    int idle_s = getenv("LXRT_BINDER_HUB_IDLE") ? atoi(getenv("LXRT_BINDER_HUB_IDLE")) : 10;
    if (idle_s < 1) idle_s = 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t want = rl.rlim_max < 10240 ? rl.rlim_max : 10240;
        if (rl.rlim_cur < want) { rl.rlim_cur = want; setrlimit(RLIMIT_NOFILE, &rl); }
    }
    LIST_INIT(&g_procs);
    LIST_INIT(&g_dead_nodes);

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s/hub.sock", g_dir) >= (int)sizeof sa.sun_path) {
        fprintf(stderr, "binder hub: %s: path too long\n", g_dir);
        return 1;
    }
    // One hub per directory: hub.pid is held locked for the hub's whole life
    // (the daemon inherits this descriptor) and let go only after the socket
    // is gone, as the property service does with service.lock. A client
    // whose connect failed while a hub still held it (a listen backlog full
    // while the hub was busy, a hub leaving) used to start a second one,
    // which unlinked the live hub's socket and bound its own: two drivers,
    // and processes on the new one found no context manager. A hub that is
    // leaving holds the lock a moment longer: wait a little.
    char lockp[1024];
    snprintf(lockp, sizeof lockp, "%s/hub.pid", g_dir);
    int life = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    bool locked = false;
    for (int i = 0; life >= 0 && i < 100 && !locked; i++) {    // up to 10 s
        if (flock(life, LOCK_EX | LOCK_NB) == 0) locked = true;
        else usleep(100000);
    }
    if (!locked) {
        if (life < 0) fprintf(stderr, "binder hub: %s: %s\n", lockp, strerror(errno));
        else fprintf(stderr, "binder hub: another hub holds %s\n", lockp);
        return 1;
    }
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { perror("binder hub: socket"); return 1; }
    fcntl(ls, F_SETFD, FD_CLOEXEC);
    unlink(sa.sun_path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
        perror("binder hub: bind");
        return 1;
    }
    chmod(sa.sun_path, 0600);
    struct stat sock_st;
    stat(sa.sun_path, &sock_st);

    if (daemonize) {
        // The spawner waits for this first process: once it exits, the socket
        // is listening, and the hub itself is no child of any guest (a guest's
        // wait(-1) must never see it).
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) _exit(0);
        setsid();
        char logp[1024];
        snprintf(logp, sizeof logp, "%s/hub.log", g_dir);
        struct stat lst;
        int lfl = O_WRONLY | O_CREAT | O_APPEND;
        if (stat(logp, &lst) == 0 && lst.st_size > (1 << 20)) lfl |= O_TRUNC;
        int lfd = open(logp, lfl, 0600);
        if (lfd >= 0) { dup2(lfd, 2); if (lfd != 2) close(lfd); }
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
    if (ftruncate(life, 0) == 0)
        dprintf(life, "%d\n", (int)getpid());
    hub_log("started, pid %d, %s", (int)getpid(), sa.sun_path);

    time_t idle_since = time(NULL);
    struct pollfd *pfd = NULL;
    void **who = NULL;
    size_t cap = 0;
    while (!g_stop) {
        size_t n = 1;
        struct list *pe, *te;
        for (pe = g_procs.next; pe != &g_procs; pe = pe->next) {
            struct proc *p = container_of(pe, struct proc, entry);
            n += 1;
            for (te = p->threads.next; te != &p->threads; te = te->next) n++;
        }
        if (n > cap) {
            cap = n * 2;
            pfd = realloc(pfd, cap * sizeof *pfd);
            who = realloc(who, cap * sizeof *who);
            if (!pfd || !who) { hub_log("out of memory"); return 70; }
        }
        size_t k = 0;
        pfd[k] = (struct pollfd){ ls, POLLIN, 0 }; who[k++] = NULL;
        for (pe = g_procs.next; pe != &g_procs; pe = pe->next) {
            struct proc *p = container_of(pe, struct proc, entry);
            pfd[k] = (struct pollfd){ p->fd, POLLIN, 0 }; who[k++] = p;
            for (te = p->threads.next; te != &p->threads; te = te->next) {
                struct thread *t = container_of(te, struct thread, proc_entry);
                pfd[k] = (struct pollfd){ t->fd, POLLIN, 0 }; who[k++] = t;
            }
        }
        bool empty = list_empty(&g_procs);
        int timeout = empty ? 1000 : -1;
        int r = poll(pfd, (nfds_t)k, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            hub_log("poll: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            if (empty && time(NULL) - idle_since >= idle_s)
                break;
            continue;
        }
        // Handle one ready descriptor at a time, re-polling after each: any
        // event can free the procs and threads the array names.
        static size_t rot;
        size_t i = k;
        for (size_t j = 0; j < k; j++) {
            size_t x = (rot + j) % k;
            if (pfd[x].revents) { i = x; break; }
        }
        if (i == k) continue;
        rot = i + 1;
        if (i == 0) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                struct timeval tv = { 10, 0 };
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                struct proc *p = xcalloc(1, sizeof *p);
                p->fd = c;
                p->id = ++g_proc_id;
                LIST_INIT(&p->threads); LIST_INIT(&p->nodes); LIST_INIT(&p->refs);
                LIST_INIT(&p->todo); LIST_INIT(&p->delivered_death); LIST_INIT(&p->waiting_threads);
                LIST_INIT(&p->buffers);
                list_add_tail(&p->entry, &g_procs);
            }
        } else {
            // Find the object again (the arrays are rebuilt every round).
            void *obj = who[i];
            bool is_proc = false;
            for (pe = g_procs.next; pe != &g_procs; pe = pe->next)
                if (container_of(pe, struct proc, entry) == obj) { is_proc = true; break; }
            if (is_proc) proc_message((struct proc *)obj);
            else thread_message((struct thread *)obj);
        }
        settle();
        if (!list_empty(&g_procs) || !empty) idle_since = time(NULL);
    }
    // Leave the name to a successor only if it is still ours.
    struct stat now;
    if (stat(sa.sun_path, &now) == 0 && now.st_ino == sock_st.st_ino)
        unlink(sa.sun_path);
    close(ls);
    hub_log("exiting (%s)", g_stop ? "signal" : "idle");
    return 0;
}
