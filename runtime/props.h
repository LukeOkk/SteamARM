// Android system properties for lxrun guests, without init and without a VM.
//
// Two halves (docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Properties"):
//
//   runtime/propsvc.c  the property service, a host process per Android root
//                      (`lxrun --property-service <dir> <root>`): it builds
//                      /dev/__properties__ (property_info and one 128 KiB
//                      area per SELinux context, bionic's own formats) from
//                      the image's property_contexts and .prop files, as
//                      init's property service does, and serves
//                      /dev/socket/property_service (setprop);
//   runtime/props.c    in every lxrun process: those two guest paths lead to
//                      the service's directory, the service is started on
//                      demand, and the area files look root-owned to fstat
//                      (bionic maps nothing else).
//
// Guests read properties exactly as on Android: they map the area files
// read-only (MAP_SHARED) and follow bionic's serial protocol. The service
// writes through its own shared mapping of the same files, and wakes
// __system_property_wait sleepers with a process-shared ulock wake on the
// serial word, the queue runtime/futex_ops.c parks shared futexes on.
#ifndef LXRT_PROPS_H
#define LXRT_PROPS_H

#include <stdbool.h>
#include <stddef.h>

struct stat;

// A guest path under /dev/__properties__ or /dev/socket/property_service:
// the host path in buf (the areas are built first if they do not exist yet).
// NULL for every other path, or when this process has no property service
// (no LXRT_ROOT, no plat_property_contexts in it, LXRT_PROPERTY_SERVICE=0,
// or a property directory that is not this user's private one).
const char *lxrt_props_translate(const char *path, char *buf, size_t n);

// The area files are root:root on Android and bionic refuses them otherwise
// (prop_area.cpp map_fd_ro, property_info_parser.cpp LoadPath): report
// uid 0 and gid 0 for the property directory and what is in it. `host` is
// the host path when known, else fd is asked.
void lxrt_props_fix_stat(const char *host, int fd, struct stat *st);

// connect() to /dev/socket/property_service found no listener: start the
// service (under a lock, once) so the caller can retry. True when it is up.
bool lxrt_props_is_socket(const char *guest_path);
bool lxrt_props_start_service(void);

// propsvc.c: `lxrun --property-service <dir> <root> [--daemon]`
int lxrt_property_service_main(int argc, char **argv);

#endif
