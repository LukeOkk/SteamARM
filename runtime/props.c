// The guest side of Android's system properties (runtime/props.h): the two
// guest paths bionic uses lead to the property service's directory, the
// service is started when a guest first needs it, and the area files look
// root-owned.
//
//   /dev/__properties__[/...]       -> <dir>/__properties__[/...]
//   /dev/socket/property_service    -> <dir>/property_service
//
// <dir> is LXRT_PROPERTY_DIR, else /tmp/lxrt-props-<uid>-<hash of the
// root's real path>: one property state per Android root (the areas are
// built from that root's own files). Like the binder hub's, it must be this
// user's own and closed to others, or no property service is offered.
//
// bionic reads the areas at every process start (__libc_init_common ->
// __system_properties_init: stat of the directory, access of property_info)
// and maps them lazily per context; nothing else is intercepted. The
// service is started by the first process that finds no property_info, and
// again by a connect() to the socket that finds nobody listening
// (runtime/socket.c), so a service that left when idle comes back for the
// next setprop.
#include "props.h"
#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
bool lxrt_trace_on(void);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(props_lock, g_lock)
static int g_state;                  // 0 not looked at, 1 offered, -1 not
static bool g_areas_tried;           // looked for the areas (and started the service)
static char g_dir[256];
static char g_areas[300];
static char g_root[PATH_MAX];
static dev_t g_dev;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001b3ull; }
    return h;
}

// Caller holds g_lock.
static bool offered_locked(void)
{
    if (g_state)
        return g_state > 0;
    g_state = -1;
    const char *root = getenv("LXRT_ROOT");
    const char *off = getenv("LXRT_PROPERTY_SERVICE");
    if (!root || !*root || (off && !strcmp(off, "0")) || !realpath(root, g_root))
        return false;
    // The service builds everything from these; a root without them is not
    // an Android root (or not one this service knows).
    char probe[PATH_MAX + 64];
    snprintf(probe, sizeof probe, "%s/system/etc/selinux/plat_property_contexts", g_root);
    if (access(probe, R_OK) != 0)
        return false;
    const char *e = getenv("LXRT_PROPERTY_DIR");
    if (e && *e) snprintf(g_dir, sizeof g_dir, "%s", e);
    else snprintf(g_dir, sizeof g_dir, "/tmp/lxrt-props-%u-%016llx", (unsigned)getuid(),
                  (unsigned long long)fnv1a64(g_root));
    mkdir(g_dir, 0700);
    struct stat st;
    if (lstat(g_dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        fprintf(lxrt_trace_stream(), "[lxrt] properties: %s is not a private directory of this "
                                     "user; no property service\n", g_dir);
        return false;
    }
    // From here on the real path: the service and every guest name the same
    // files the same way whatever their working directory, and it is what
    // F_GETPATH answers (/tmp is /private/tmp).
    char canon[PATH_MAX];
    if (!realpath(g_dir, canon) || snprintf(g_dir, sizeof g_dir, "%s", canon) >= (int)sizeof g_dir)
        return false;
    snprintf(g_areas, sizeof g_areas, "%s/__properties__", g_dir);
    g_dev = st.st_dev;
    g_state = 1;
    return true;
}

static const char *self_exe(void)
{
    static char path[4096];
    if (path[0]) return path;
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) path[0] = 0;
    return path;
}

// Is a service serving? It holds service.lock for its whole life and
// removes its socket before it lets go of the lock, so "lock held and
// socket present" is a service that answers; "lock held, no socket" is one
// that is leaving (wait for it); "lock free" is none.
static int service_state(void)
{
    char lp[PATH_MAX], sp[PATH_MAX];
    snprintf(lp, sizeof lp, "%s/service.lock", g_dir);
    snprintf(sp, sizeof sp, "%s/property_service", g_dir);
    int fd = open(lp, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close(fd);
    struct stat st;
    if (!held) return 0;                                    // none
    return stat(sp, &st) == 0 && S_ISSOCK(st.st_mode) ? 1   // serving
                                                      : -1; // leaving
}

static bool socket_answers(void)
{
    for (int i = 0; i < 1000; i++) {            // a leaving service: up to 10 s
        int s = service_state();
        if (s >= 0) return s == 1;
        usleep(10000);
    }
    return false;
}

static bool areas_exist(void)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/property_info", g_areas);
    return access(p, R_OK) == 0;
}

// `lxrun --property-service <dir> <root> --daemon`: its first process builds
// or adopts the areas, binds the socket, forks the service and exits, so
// once waitpid returns both are ready. Spawners serialise on spawn.lock.
// Caller holds g_lock.
static bool spawn_locked(bool for_socket)
{
    char lp[PATH_MAX];
    snprintf(lp, sizeof lp, "%s/spawn.lock", g_dir);
    int lk = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk >= 0) flock(lk, LOCK_EX);
    bool ready = for_socket ? socket_answers() : areas_exist();
    if (!ready) {
        const char *exe = self_exe();
        posix_spawnattr_t at;
        posix_spawn_file_actions_t fa;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        char *env[12];
        int ne = 0;
        for (char **ep = environ; *ep && ne < 10; ep++)
            if (!strncmp(*ep, "LXRT_PROPERTY_", 14)) env[ne++] = *ep;
        env[ne] = NULL;
        char *argv[] = { (char *)exe, "--property-service", g_dir, g_root, "--daemon", NULL };
        pid_t pid;
        int rc = exe[0] ? posix_spawn(&pid, exe, &fa, &at, argv, env) : ENOENT;
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        if (rc == 0) {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
        } else if (lxrt_trace_on()) {
            fprintf(lxrt_trace_stream(), "[lxrt] properties: cannot start the service: %s\n", strerror(rc));
        }
        ready = for_socket ? socket_answers() : areas_exist();
        if (!ready)
            fprintf(lxrt_trace_stream(), "[lxrt] properties: the service did not start (%s/service.log)\n", g_dir);
    }
    if (lk >= 0) { flock(lk, LOCK_UN); close(lk); }
    return ready;
}

const char *lxrt_props_translate(const char *path, char *buf, size_t n)
{
    if (!path || strncmp(path, "/dev/", 5) != 0)
        return NULL;
    bool areas = !strncmp(path, "/dev/__properties__", 19) && (path[19] == '\0' || path[19] == '/');
    bool sock = !strcmp(path, "/dev/socket/property_service");
    if (!areas && !sock)
        return NULL;
    pthread_mutex_lock(&g_lock);
    bool on = offered_locked();
    if (on && areas && !g_areas_tried) {
        // Once per process: there are areas, or this process starts the
        // service that builds them (and does not try again if it fails).
        g_areas_tried = true;
        if (!areas_exist())
            spawn_locked(false);
    }
    pthread_mutex_unlock(&g_lock);
    if (!on)
        return NULL;
    int k = areas ? snprintf(buf, n, "%s%s", g_areas, path + 19)
                  : snprintf(buf, n, "%s/property_service", g_dir);
    return k > 0 && (size_t)k < n ? buf : NULL;
}

bool lxrt_props_is_socket(const char *guest_path)
{
    return guest_path && !strcmp(guest_path, "/dev/socket/property_service");
}

bool lxrt_props_start_service(void)
{
    pthread_mutex_lock(&g_lock);
    bool ok = offered_locked() && spawn_locked(true);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static bool under(const char *p, const char *dir)
{
    size_t l = strlen(dir);
    return !strncmp(p, dir, l) && (p[l] == '\0' || p[l] == '/');
}

void lxrt_props_fix_stat(const char *host, int fd, struct stat *st)
{
    // Only a process that has looked at /dev/__properties__ can hold one of
    // its files, and the device number rules out nearly everything else
    // without a path lookup.
    if (g_state <= 0 || st->st_dev != g_dev)
        return;
    char p[PATH_MAX];
    if (!host) {
        if (fd < 0 || fcntl(fd, F_GETPATH, p) != 0)
            return;
        host = p;
    }
    if (under(host, g_areas)) {
        st->st_uid = 0;
        st->st_gid = 0;
    }
}
