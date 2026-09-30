// Android binder for lxrun guests, without a Linux kernel.
//
// Two halves (docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Binder"):
//
//   runtime/binder.c      in every lxrun process: /dev/binder, /dev/hwbinder
//                         and /dev/vndbinder as descriptors whose ioctl, mmap
//                         and poll are answered here;
//   runtime/binder_hub.c  one host process per user (lxrun --binder-hub): the
//                         driver itself -- nodes, references, handles,
//                         transactions, death notifications, thread queues and
//                         each process's receive buffer -- ported from the
//                         Linux driver's semantics (drivers/android/binder.c).
//
// Guest processes are separate macOS processes, so binder is inter-process
// here exactly as on Linux; the hub is where the kernel would be. This file
// holds the Linux UAPI (include/uapi/linux/android/binder.h, 64-bit
// layout, protocol version 8) and the private wire protocol between the two
// halves.
#ifndef LXRT_BINDER_H
#define LXRT_BINDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------------ Linux UAPI
//
// Linux's _IOC encoding. Darwin's <sys/ioccom.h> puts IN and OUT the other
// way round (_IOW is 0x80000000 there, 0x40000000 on Linux), so the Linux
// macros are spelled out rather than borrowed.
#define LX_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr))
#define LX_IO(t, n)      LX_IOC(0, (t), (n), 0)
#define LX_IOW(t, n, s)  LX_IOC(1, (t), (n), (s))
#define LX_IOR(t, n, s)  LX_IOC(2, (t), (n), (s))
#define LX_IOWR(t, n, s) LX_IOC(3, (t), (n), (s))
#define LX_IOC_SIZE(c)   (((c) >> 16) & 0x3fff)

#define B_PACK_CHARS(c1, c2, c3, c4) \
    ((((uint32_t)(c1)) << 24) | (((uint32_t)(c2)) << 16) | (((uint32_t)(c3)) << 8) | (uint32_t)(c4))
#define B_TYPE_LARGE 0x85
enum {
    BINDER_TYPE_BINDER      = B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),
    BINDER_TYPE_WEAK_BINDER = B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),
    BINDER_TYPE_HANDLE      = B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),
    BINDER_TYPE_WEAK_HANDLE = B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),
    BINDER_TYPE_FD          = B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),
    BINDER_TYPE_FDA         = B_PACK_CHARS('f', 'd', 'a', B_TYPE_LARGE),
    BINDER_TYPE_PTR         = B_PACK_CHARS('p', 't', '*', B_TYPE_LARGE),
};

#define FLAT_BINDER_FLAG_PRIORITY_MASK    0xff
#define FLAT_BINDER_FLAG_ACCEPTS_FDS      0x100
#define FLAT_BINDER_FLAG_TXN_SECURITY_CTX 0x1000
#define BINDER_BUFFER_FLAG_HAS_PARENT     0x01

struct binder_object_header { uint32_t type; };

struct flat_binder_object {
    struct binder_object_header hdr;
    uint32_t flags;
    union { uint64_t binder; uint32_t handle; };
    uint64_t cookie;
};
struct binder_fd_object {
    struct binder_object_header hdr;
    uint32_t pad_flags;
    union { uint64_t pad_binder; uint32_t fd; };
    uint64_t cookie;
};
struct binder_buffer_object {
    struct binder_object_header hdr;
    uint32_t flags;
    uint64_t buffer;
    uint64_t length;
    uint64_t parent;
    uint64_t parent_offset;
};
struct binder_fd_array_object {
    struct binder_object_header hdr;
    uint32_t pad;
    uint64_t num_fds;
    uint64_t parent;
    uint64_t parent_offset;
};

struct binder_write_read {
    uint64_t write_size;
    uint64_t write_consumed;
    uint64_t write_buffer;
    uint64_t read_size;
    uint64_t read_consumed;
    uint64_t read_buffer;
};
struct binder_version { int32_t protocol_version; };
#define BINDER_CURRENT_PROTOCOL_VERSION 8

struct binder_node_debug_info {
    uint64_t ptr;
    uint64_t cookie;
    uint32_t has_strong_ref;
    uint32_t has_weak_ref;
};
struct binder_node_info_for_ref {
    uint32_t handle;
    uint32_t strong_count;
    uint32_t weak_count;
    uint32_t reserved1, reserved2, reserved3;
};

#define BINDER_WRITE_READ             LX_IOWR('b', 1, sizeof(struct binder_write_read))
#define BINDER_SET_IDLE_TIMEOUT       LX_IOW('b', 3, 8)
#define BINDER_SET_MAX_THREADS        LX_IOW('b', 5, 4)
#define BINDER_SET_IDLE_PRIORITY      LX_IOW('b', 6, 4)
#define BINDER_SET_CONTEXT_MGR        LX_IOW('b', 7, 4)
#define BINDER_THREAD_EXIT            LX_IOW('b', 8, 4)
#define BINDER_VERSION                LX_IOWR('b', 9, sizeof(struct binder_version))
#define BINDER_GET_NODE_DEBUG_INFO    LX_IOWR('b', 11, sizeof(struct binder_node_debug_info))
#define BINDER_GET_NODE_INFO_FOR_REF  LX_IOWR('b', 12, sizeof(struct binder_node_info_for_ref))
#define BINDER_SET_CONTEXT_MGR_EXT    LX_IOW('b', 13, sizeof(struct flat_binder_object))

enum transaction_flags {
    TF_ONE_WAY     = 0x01,
    TF_ROOT_OBJECT = 0x04,
    TF_STATUS_CODE = 0x08,
    TF_ACCEPT_FDS  = 0x10,
    TF_CLEAR_BUF   = 0x20,
};

struct binder_transaction_data {
    union { uint32_t handle; uint64_t ptr; } target;
    uint64_t cookie;
    uint32_t code;
    uint32_t flags;
    int32_t  sender_pid;
    uint32_t sender_euid;
    uint64_t data_size;
    uint64_t offsets_size;
    union {
        struct { uint64_t buffer; uint64_t offsets; } ptr;
        uint8_t buf[8];
    } data;
};
struct binder_transaction_data_secctx {
    struct binder_transaction_data transaction_data;
    uint64_t secctx;
};
struct binder_transaction_data_sg {
    struct binder_transaction_data transaction_data;
    uint64_t buffers_size;
};
struct binder_ptr_cookie { uint64_t ptr; uint64_t cookie; };
struct __attribute__((packed)) binder_handle_cookie { uint32_t handle; uint64_t cookie; };

_Static_assert(sizeof(struct flat_binder_object) == 24, "flat_binder_object");
_Static_assert(sizeof(struct binder_buffer_object) == 40, "binder_buffer_object");
_Static_assert(sizeof(struct binder_fd_array_object) == 32, "binder_fd_array_object");
_Static_assert(sizeof(struct binder_transaction_data) == 64, "binder_transaction_data");
_Static_assert(sizeof(struct binder_write_read) == 48, "binder_write_read");
_Static_assert(sizeof(struct binder_handle_cookie) == 12, "binder_handle_cookie");

enum binder_driver_return_protocol {
    BR_ERROR                 = LX_IOR('r', 0, 4),
    BR_OK                    = LX_IO('r', 1),
    BR_TRANSACTION_SEC_CTX   = LX_IOR('r', 2, sizeof(struct binder_transaction_data_secctx)),
    BR_TRANSACTION           = LX_IOR('r', 2, sizeof(struct binder_transaction_data)),
    BR_REPLY                 = LX_IOR('r', 3, sizeof(struct binder_transaction_data)),
    BR_ACQUIRE_RESULT        = LX_IOR('r', 4, 4),
    BR_DEAD_REPLY            = LX_IO('r', 5),
    BR_TRANSACTION_COMPLETE  = LX_IO('r', 6),
    BR_INCREFS               = LX_IOR('r', 7, sizeof(struct binder_ptr_cookie)),
    BR_ACQUIRE               = LX_IOR('r', 8, sizeof(struct binder_ptr_cookie)),
    BR_RELEASE               = LX_IOR('r', 9, sizeof(struct binder_ptr_cookie)),
    BR_DECREFS               = LX_IOR('r', 10, sizeof(struct binder_ptr_cookie)),
    BR_ATTEMPT_ACQUIRE       = LX_IOR('r', 11, 24),
    BR_NOOP                  = LX_IO('r', 12),
    BR_SPAWN_LOOPER          = LX_IO('r', 13),
    BR_FINISHED              = LX_IO('r', 14),
    BR_DEAD_BINDER           = LX_IOR('r', 15, 8),
    BR_CLEAR_DEATH_NOTIFICATION_DONE = LX_IOR('r', 16, 8),
    BR_FAILED_REPLY          = LX_IO('r', 17),
};

enum binder_driver_command_protocol {
    BC_TRANSACTION           = LX_IOW('c', 0, sizeof(struct binder_transaction_data)),
    BC_REPLY                 = LX_IOW('c', 1, sizeof(struct binder_transaction_data)),
    BC_ACQUIRE_RESULT        = LX_IOW('c', 2, 4),
    BC_FREE_BUFFER           = LX_IOW('c', 3, 8),
    BC_INCREFS               = LX_IOW('c', 4, 4),
    BC_ACQUIRE               = LX_IOW('c', 5, 4),
    BC_RELEASE               = LX_IOW('c', 6, 4),
    BC_DECREFS               = LX_IOW('c', 7, 4),
    BC_INCREFS_DONE          = LX_IOW('c', 8, sizeof(struct binder_ptr_cookie)),
    BC_ACQUIRE_DONE          = LX_IOW('c', 9, sizeof(struct binder_ptr_cookie)),
    BC_ATTEMPT_ACQUIRE       = LX_IOW('c', 10, 8),
    BC_REGISTER_LOOPER       = LX_IO('c', 11),
    BC_ENTER_LOOPER          = LX_IO('c', 12),
    BC_EXIT_LOOPER           = LX_IO('c', 13),
    BC_REQUEST_DEATH_NOTIFICATION = LX_IOW('c', 14, sizeof(struct binder_handle_cookie)),
    BC_CLEAR_DEATH_NOTIFICATION   = LX_IOW('c', 15, sizeof(struct binder_handle_cookie)),
    BC_DEAD_BINDER_DONE      = LX_IOW('c', 16, 8),
    BC_TRANSACTION_SG        = LX_IOW('c', 17, sizeof(struct binder_transaction_data_sg)),
    BC_REPLY_SG              = LX_IOW('c', 18, sizeof(struct binder_transaction_data_sg)),
};

// ------------------------------------------------------------ wire protocol
//
// Every lxrun process that opens a binder device holds one "process channel"
// (the descriptor the guest sees: a Unix stream socket to the hub) and, per
// guest thread that uses it, one "thread channel" (a socketpair whose other
// end was handed to the hub over the process channel). Requests and results
// travel on the thread channel, one result per request, in order. The only
// bytes the hub writes on the process channel after the handshake are
// doorbells: one byte each, making the guest's descriptor readable when work
// waits for a thread that polls it (Linux binder_poll). A result says how many
// doorbells have been written so far and whether one should still be
// pending; the runtime drains to that count, so readability tracks the
// driver's state instead of drifting.
#define BH_VERSION 1
#define BH_MAX_FDS 250          // file descriptors in one message (SCM_RIGHTS)
#define BH_MAX_MSG (8u << 20)   // no request is larger than 2 receive buffers

enum bh_type {
    BH_HELLO = 1,       // proc channel, runtime -> hub: struct bh_hello
    BH_HELLO_ACK,       // proc channel, hub -> runtime: struct bh_hello_ack
    BH_THREAD,          // proc channel, runtime -> hub: struct bh_thread + 1 fd
    BH_WRITE_READ,      // thread channel: struct bh_wr + write bytes + attachments
    BH_CANCEL,          // thread channel: the blocked WRITE_READ `seq` was interrupted
    BH_IOCTL,           // thread channel: struct bh_ioctl
    BH_MMAP,            // thread channel: struct bh_mmap + 1 fd (the buffer's file)
    BH_POLL,            // thread channel: this thread polls the descriptor
    BH_RESULT,          // thread channel, hub -> runtime: struct bh_result + ...
};

struct bh_hdr {
    uint32_t type;
    uint32_t nfds;      // descriptors carried with this message (SCM_RIGHTS)
    uint64_t len;       // payload bytes after this header
    uint64_t seq;
};

struct bh_hello {
    uint32_t version;
    int32_t  pid;       // the guest's pid (the lxrun process)
    uint32_t euid;
    uint32_t context;   // 0 binder, 1 hwbinder, 2 vndbinder
    char     secctx[64];
};
struct bh_hello_ack { int32_t ret; uint32_t proc_id; };
struct bh_thread { int32_t tid; uint32_t pad; };

#define BH_WR_NONBLOCK 1
struct bh_wr {
    uint64_t write_len;       // write bytes that follow (from write_consumed on)
    uint64_t read_avail;      // read_size - read_consumed
    uint64_t read_consumed;   // as the guest passed it (0: the read starts with BR_NOOP)
    uint32_t flags;           // BH_WR_*
    uint32_t pad;
};
// One per BC_TRANSACTION(_SG)/BC_REPLY(_SG) in the write bytes, in order,
// after them: the sender's data, offsets and scatter-gather buffers (each
// padded to 8), and `nfds` descriptors in object order. `fault` is a Linux
// errno when the sender's memory could not be read or an fd was invalid.
struct bh_txn_att {
    int32_t  fault;
    uint32_t nfds;
    uint64_t data_size;
    uint64_t offsets_size;
    uint64_t sg_size;
};
// Not a Linux ioctl: the runtime telling the hub the guest unmapped its
// receive buffer (Linux: binder_vma_close).
#define BH_IOCTL_UNMAPPED 0xb1d0001u
struct bh_ioctl {
    uint32_t cmd;             // the Linux ioctl number, or BH_IOCTL_UNMAPPED
    uint32_t pad;
    uint8_t  arg[64];
};
struct bh_mmap { uint64_t base; uint64_t size; };

// Result of any thread-channel request.
struct bh_result {
    int64_t  ret;             // 0 or -errno (Linux numbering)
    uint64_t write_consumed;  // bytes of this request's write data consumed
    uint64_t read_len;        // BR bytes that follow
    uint32_t spawn_looper;    // put BR_SPAWN_LOOPER at the read buffer's start
    uint32_t nfixups;         // u64 buffer offsets that follow the BR bytes
    uint32_t ncloses;         // i32 descriptors to close (freed FDA buffers)
    uint32_t bell_pending;    // a doorbell should remain unread
    uint64_t bells;           // doorbells written on the process channel so far
    uint8_t  arg[64];         // BH_IOCTL's output
};

// ------------------------------------------------------------ runtime API
// (binder.c; dispatch.c routes the guest's syscalls here)
struct stat;
int  lxrt_binder_context_of(const char *guest_path);  // -1: not a binder device
long lxrt_binder_open(int context, int lflags);
bool lxrt_binder_is(int fd);
long lxrt_binder_ioctl(int fd, unsigned long req, uint64_t arg);
long lxrt_binder_mmap(uint64_t addr, uint64_t len, int prot, int lflags, int fd, uint64_t off);
bool lxrt_binder_munmap(uint64_t addr, uint64_t len, long *ret);   // closes the buffers in the range; the caller still unmaps it
void lxrt_binder_close(int fd);
void lxrt_binder_dup(int oldfd, int newfd);
void lxrt_binder_polled(int fd);
void lxrt_binder_thread_exit(void);
bool lxrt_binder_any(void);            // has this process ever opened one?
bool lxrt_binder_stat(const char *guest_path, struct stat *st);
bool lxrt_binder_fstat(int fd, struct stat *st);
// binder_hub.c: `lxrun --binder-hub <dir> [--daemon]`
int  lxrt_binder_hub_main(int argc, char **argv);

#endif
