#include "ids.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <unistd.h>

#define IDS_COUNT 65536
struct id_entry {
    _Atomic int32_t pid;       // 0 free, -1 being filled, positive host pid
    _Atomic int32_t thread;    // zero is process leader
    _Atomic uint32_t generation;
};
struct id_table {
    _Atomic uint32_t cursor;
    struct id_entry entries[IDS_COUNT];
};
static struct id_table *g_ids;
static _Atomic int g_state;    // 0 uninitialized, 1 ready, -1 failed
static int g_pid;
// A child can release its shared slot before wait(2) reaps it. The parent's
// private map keeps the fork return value stable until that wait completes.
static _Atomic int g_children[IDS_COUNT]; // guest ID -> host PID

// Darwin recycles PIDs.  A slot from an abruptly killed old process must
// never become an ID of a new, unrelated process that got the same host PID.
static uint32_t birth(int pid)
{
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != sizeof bi)
        return 0;
    uint64_t t = (uint64_t)bi.pbi_start_tvsec * 1000000ull + bi.pbi_start_tvusec;
    uint32_t h = (uint32_t)(t ^ (t >> 32));
    return h ? h : 1;
}

// Read once: every getpid, gettid and kill asks, and lxrun's own environment
// (not the guest's) does not change while it runs.
bool lxrt_ids_on(void)
{
    static _Atomic int on = -1;
    int v = atomic_load_explicit(&on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("LXRT_SMALL_IDS");
        v = e && e[0] == '1' && e[1] == '\0';
        atomic_store_explicit(&on, v, memory_order_relaxed);
    }
    return v;
}

static void ids_atexit(void) { lxrt_ids_release_process(); }

// O_EXCL lets the creator size the file before any other process maps it.
// Existing files must belong to us, be regular, and have no group access.
static bool open_table(void)
{
    char path[96];
    snprintf(path, sizeof path, "/tmp/lxrt-ids-%u", (unsigned)getuid());
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (ftruncate(fd, sizeof(struct id_table)) != 0) {
            close(fd); unlink(path); return false;
        }
    } else if (errno == EEXIST) {
        fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) return false;
    struct stat st;
    bool valid = false;
    for (int i = 0; i < 100; i++) {
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
            !(st.st_mode & 0077) && st.st_size == sizeof(struct id_table)) {
            valid = true; break;
        }
        usleep(1000);
    }
    if (valid) {
        void *p = mmap(NULL, sizeof(struct id_table), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) g_ids = p;
    }
    close(fd);
    return g_ids != NULL;
}

static int claim(int host_pid, int index)
{
    if (!g_ids) return -1;
    uint32_t generation = host_pid > 0 ? birth(host_pid) : 0;
    if (host_pid > 0 && !generation) return -1;
    uint32_t start = atomic_fetch_add(&g_ids->cursor, 1);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t n = 0; n < IDS_COUNT - 2; n++) {
            int id = 2 + (int)((start + n) % (IDS_COUNT - 2));
            struct id_entry *e = &g_ids->entries[id];
            int32_t old = atomic_load_explicit(&e->pid, memory_order_acquire);
            if (old && pass && old > 0 &&
                (birth(old) != atomic_load(&e->generation) ||
                 (kill(old, 0) < 0 && errno == ESRCH))) {
                int32_t expected = old;
                atomic_compare_exchange_strong(&e->pid, &expected, 0);
                old = atomic_load(&e->pid);
            }
            if (old) continue;
            int32_t expected = 0;
            if (!atomic_compare_exchange_strong(&e->pid, &expected, -1)) continue;
            atomic_store(&e->thread, index);
            atomic_store(&e->generation, generation);
            atomic_store_explicit(&e->pid, host_pid, memory_order_release);
            return id;
        }
    }
    return -1;
}

int lxrt_ids_init(void)
{
    if (!lxrt_ids_on()) return 0;
    if (atomic_load(&g_state) == 1) return g_pid;
    if (atomic_load(&g_state) == -1) return -1;
    if (!open_table()) { atomic_store(&g_state, -1); return -1; }
    int host = (int)getpid();
    uint32_t generation = birth(host);
    if (!generation) { atomic_store(&g_state, -1); return -1; }
    // execve replaces this runtime in the same host process. Keep its ID.
    for (int id = 2; id < IDS_COUNT; id++)
        if (atomic_load(&g_ids->entries[id].pid) == host &&
            atomic_load(&g_ids->entries[id].thread) == 0 &&
            atomic_load(&g_ids->entries[id].generation) == generation) {
            g_pid = id; break;
        }
    if (!g_pid) g_pid = claim(host, 0);
    if (g_pid < 2) { atomic_store(&g_state, -1); return -1; }
    atexit(ids_atexit);
    atomic_store(&g_state, 1);
    return g_pid;
}

int lxrt_ids_pid(void)
{
    if (!lxrt_ids_on()) return (int)getpid();
    return g_pid > 1 ? g_pid : lxrt_ids_init();
}

int lxrt_ids_alloc_thread(int index)
{
    return lxrt_ids_on() ? claim((int)getpid(), index) : 0;
}

int lxrt_ids_reserve_child(void)
{
    return lxrt_ids_on() ? claim(-1, 0) : 0;
}

void lxrt_ids_cancel_child(int id)
{
    if (!lxrt_ids_on() || !g_ids || id < 2 || id >= IDS_COUNT) return;
    int32_t expected = -1;
    atomic_compare_exchange_strong(&g_ids->entries[id].pid, &expected, 0);
}

void lxrt_ids_publish_child(int id, int host_pid)
{
    if (lxrt_ids_on() && id >= 2 && id < IDS_COUNT) {
        atomic_store(&g_ids->entries[id].generation, birth(host_pid));
        atomic_store_explicit(&g_ids->entries[id].pid, host_pid, memory_order_release);
    }
}

void lxrt_ids_child_after_fork(int id)
{
    if (!lxrt_ids_on()) return;
    g_pid = id;
    memset(g_children, 0, sizeof g_children);
    lxrt_ids_publish_child(id, (int)getpid());
}

void lxrt_ids_release(int id)
{
    if (!lxrt_ids_on() || !g_ids || id < 2 || id >= IDS_COUNT) return;
    int32_t expected = (int32_t)getpid();
    atomic_compare_exchange_strong(&g_ids->entries[id].pid, &expected, 0);
}

void lxrt_ids_release_process(void)
{
    if (!lxrt_ids_on() || !g_ids) return;
    int32_t host = (int32_t)getpid();
    for (int id = 2; id < IDS_COUNT; id++) {
        int32_t expected = host;
        atomic_compare_exchange_strong(&g_ids->entries[id].pid, &expected, 0);
    }
}

int lxrt_ids_to_guest(int host_pid, int index)
{
    if (!lxrt_ids_on() || host_pid <= 0) return host_pid;
    if (host_pid == getpid() && index == 0) return lxrt_ids_pid();
    if (!g_ids) return 0;
    uint32_t generation = birth(host_pid);
    if (!generation) return 0;
    for (int id = 2; id < IDS_COUNT; id++)
        if (atomic_load_explicit(&g_ids->entries[id].pid, memory_order_acquire) == host_pid &&
            atomic_load(&g_ids->entries[id].thread) == index &&
            atomic_load(&g_ids->entries[id].generation) == generation) return id;
    return 0;
}

int lxrt_ids_threads(int host_pid, int *out, int max)
{
    if (!lxrt_ids_on() || !g_ids) return 0;
    uint32_t generation = birth(host_pid);
    if (!generation) return 0;
    int n = 0;
    for (int id = 2; id < IDS_COUNT; id++)
        if (atomic_load_explicit(&g_ids->entries[id].pid, memory_order_acquire) == host_pid &&
            atomic_load(&g_ids->entries[id].generation) == generation) {
            if (n < max) out[n] = id;
            n++;
        }
    return n;
}

bool lxrt_ids_to_host(int id, int *host_pid, int *index)
{
    if (!lxrt_ids_on()) {
        if (host_pid) *host_pid = id;
        if (index) *index = 0;
        return id > 0;
    }
    if (!g_ids || id < 2 || id >= IDS_COUNT) return false;
    struct id_entry *e = &g_ids->entries[id];
    int pid = atomic_load_explicit(&e->pid, memory_order_acquire);
    if (pid <= 0) return false;
    if (atomic_load(&e->generation) != birth(pid)) return false;
    if (host_pid) *host_pid = pid;
    if (index) *index = atomic_load(&e->thread);
    return true;
}

int lxrt_ids_target_pid(int guest_pid)
{
    if (!lxrt_ids_on() || guest_pid <= 0) return guest_pid;
    if (guest_pid >= 2 && guest_pid < IDS_COUNT) {
        int child = atomic_load(&g_children[guest_pid]);
        if (child > 0) return child;
    }
    int host, index;
    return lxrt_ids_to_host(guest_pid, &host, &index) && index == 0 ? host : -1;
}

void lxrt_ids_note_child(int host_pid, int guest_pid)
{
    if (!lxrt_ids_on() || guest_pid < 2 || guest_pid >= IDS_COUNT) return;
    atomic_store(&g_children[guest_pid], host_pid);
}

int lxrt_ids_reaped_child(int host_pid, bool reap)
{
    if (!lxrt_ids_on()) return host_pid;
    for (int id = 2; id < IDS_COUNT; id++)
        if (atomic_load(&g_children[id]) == host_pid) {
            if (reap) atomic_store(&g_children[id], 0);
            return id;
        }
    return lxrt_ids_to_guest(host_pid, 0);
}
