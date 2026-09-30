#ifndef LXRT_IDS_H
#define LXRT_IDS_H

#include <stdbool.h>

// Guest IDs are host IDs unless LXRT_SMALL_IDS=1.  Index zero is the
// process leader; other indices identify guest threads within that process.
bool lxrt_ids_on(void);
int lxrt_ids_init(void);
int lxrt_ids_pid(void);
int lxrt_ids_alloc_thread(int index);
int lxrt_ids_reserve_child(void);
void lxrt_ids_cancel_child(int id);
void lxrt_ids_publish_child(int id, int host_pid);
void lxrt_ids_child_after_fork(int id);
void lxrt_ids_release(int id);
void lxrt_ids_release_process(void);
int lxrt_ids_to_guest(int host_pid, int index);
int lxrt_ids_threads(int host_pid, int *out, int max);
bool lxrt_ids_to_host(int guest_id, int *host_pid, int *index);
int lxrt_ids_target_pid(int guest_pid);
void lxrt_ids_note_child(int host_pid, int guest_pid);
int lxrt_ids_reaped_child(int host_pid, bool reap);

#endif
