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
// WIRING. This module answers nothing until the dispatcher calls it. As of
// this writing it is not called from anywhere: runtime/dispatch.c has no cases
// for 190..197 or 420, so a guest semget/semop/shmget still falls through to
// -ENOSYS, and runtime/sysv_ipc.c is not in the Makefile's LXRT_SRCS so it is
// not even linked. Both of those files belong to other modules and are not
// edited from here. What the integrator has to add, once:
//
//   * Makefile: append runtime/sysv_ipc.c to LXRT_SRCS.
//   * dispatch.c: LNR_semget=190, LNR_semctl=191, LNR_semtimedop=192,
//     LNR_semop=193, LNR_shmget=194, LNR_shmctl=195, LNR_shmat=196,
//     LNR_shmdt=197, LNR_semtimedop_time64=420, each dispatched to the
//     function below with x0..x4 passed through unmodified. Three of them
//     have argument rules that a normal (int)-truncating wrapper breaks:
//     semctl's fourth argument must be the RAW 64-bit x3 (it is a union
//     passed by value), shmget's size is a 64-bit x1, and shmat RETURNS an
//     address, so its result must not be narrowed to int.
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
