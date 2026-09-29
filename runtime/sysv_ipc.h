// System V IPC -- semaphores and shared memory.
//
// Linux aarch64 (asm-generic) syscall numbers this module answers, each read
// out of asm-generic/unistd.h rather than remembered:
//
//   190 semget      191 semctl      192 semtimedop   193 semop
//   194 shmget      195 shmctl      196 shmat        197 shmdt
//
// And one more that is easy to miss: 420 __NR_semtimedop_time64. On a 64-bit
// target asm-generic/unistd.h maps BOTH 192 and 420 to the same sys_semtimedop
// with the same 64-bit __kernel_timespec (read off the guest:
// `__SC_3264(__NR_semtimedop, sys_semtimedop_time32, sys_semtimedop)` at 192
// and `__SYSCALL(__NR_semtimedop_time64, sys_semtimedop)` at 420), and musl
// issues 420. Both numbers must reach lxrt_semtimedop unchanged.
//
// WIRING. runtime/sysv_ipc.c is in the Makefile's LXRT_SRCS (Makefile:35-38),
// and dispatch.c routes 190..197 here (LNR_* at dispatch.c:81-82, cases at
// dispatch.c:2859-2903) with the argument rules below: semctl gets the RAW
// 64-bit x3 (a union passed by value), shmget a 64-bit size, and shmat's
// returned address is not narrowed to int. 420 (semtimedop_time64) is NOT
// wired: it has no case in dispatch.c and falls to its -ENOSYS default
// (dispatch.c:3263-3283). glibc on aarch64 issues 192; a musl guest would hit
// the gap. Wiring it is one more case calling lxrt_semtimedop.
//
// Message queues (186..189) are deliberately absent: nothing in the measured
// Steam stack imports msgget/msgsnd/msgrcv, and Darwin's msg limits are even
// tighter than its shm ones.
//
// Every entry point takes the guest's arguments exactly as the dispatcher
// pulled them out of x0..x4, does its own pointer validation, and returns
// either a non-negative result or a NEGATED LINUX errno. Nothing here ever
// returns a Darwin errno.

#ifndef LXRT_SYSV_IPC_H
#define LXRT_SYSV_IPC_H

#include <stdint.h>

// 190. key is Linux key_t, a signed 32-bit value on both systems.
long lxrt_semget(int32_t key, int nsems, int lsemflg);

// 191. The fourth argument is a union (semun) passed by value in a register,
// so it arrives as a raw 64-bit word: a guest pointer for IPC_STAT/IPC_SET/
// GETALL/SETALL, and an int in the low 32 bits for SETVAL. The caller must
// pass x3 unmodified. lcmd still carries glibc's IPC_64 bit; this module
// strips it.
long lxrt_semctl(int semid, int semnum, int lcmd, uint64_t arg);

// 193.
long lxrt_semop(int semid, const void *lsops, uint64_t nsops);

// 192, and 420 (semtimedop_time64) -- the same entry point for both, because
// on a 64-bit target the kernel gives them the same handler and the same
// struct. ltimeout is a guest `struct __kernel_timespec *` (two 64-bit
// fields), or NULL, in which case this is exactly semop.
long lxrt_semtimedop(int semid, const void *lsops, uint64_t nsops,
                     const void *ltimeout);

// 194.
long lxrt_shmget(int32_t key, uint64_t size, int lshmflg);

// 196. Returns the attach address on success, so the dispatcher must not
// truncate it to int. shmaddr is the guest's requested address, or 0.
long lxrt_shmat(int shmid, uint64_t shmaddr, int lshmflg);

// 197.
long lxrt_shmdt(uint64_t shmaddr);

// 195. lbuf is a guest `struct shmid64_ds *`, or NULL for IPC_RMID.
long lxrt_shmctl(int shmid, int lcmd, void *lbuf);

#endif
