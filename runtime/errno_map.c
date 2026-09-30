// Darwin errno values are not Linux errno values.
//
// Both inherit the first 34 from BSD, and then diverge completely -- including
// a pair that is simply swapped: EAGAIN is 35 on Darwin and 11 on Linux, while
// EDEADLK is 11 on Darwin and 35 on Linux. Returning a raw Darwin errno to a
// Linux guest therefore does not produce a wrong-but-related error; it produces
// a different error entirely.
//
// This was found the hard way: the runtime returned -ENOSYS (78 on Darwin) for
// an unimplemented syscall, glibc read 78 as EREMCHG, and its clone3 -> clone
// fallback -- which triggers only on ENOSYS -- never fired. The visible symptom
// was "pthread_create: Remote address changed".
//
// The table is written with the Darwin macro on the left so the compiler
// supplies the host value, and the Linux number as a literal on the right.

#include "lxrt.h"

#include <errno.h>

int lxrt_errno_to_linux(int e)
{
    switch (e) {
    // Identical on both, kept explicit so the range is documented rather than
    // assumed.
    case EPERM:           return 1;
    case ENOENT:          return 2;
    case ESRCH:           return 3;
    case EINTR:           return 4;
    case EIO:             return 5;
    case ENXIO:           return 6;
    case E2BIG:           return 7;
    case ENOEXEC:         return 8;
    case EBADF:           return 9;
    case ECHILD:          return 10;
    case ENOMEM:          return 12;
    case EACCES:          return 13;
    case EFAULT:          return 14;
    case ENOTBLK:         return 15;
    case EBUSY:           return 16;
    case EEXIST:          return 17;
    case EXDEV:           return 18;
    case ENODEV:          return 19;
    case ENOTDIR:         return 20;
    case EISDIR:          return 21;
    case EINVAL:          return 22;
    case ENFILE:          return 23;
    case EMFILE:          return 24;
    case ENOTTY:          return 25;
    case ETXTBSY:         return 26;
    case EFBIG:           return 27;
    case ENOSPC:          return 28;
    case ESPIPE:          return 29;
    case EROFS:           return 30;
    case EMLINK:          return 31;
    case EPIPE:           return 32;
    case EDOM:            return 33;
    case ERANGE:          return 34;

    // The swapped pair, and everything after it.
    case EAGAIN:          return 11;   // Darwin 35, Linux 11
    case EDEADLK:         return 35;   // Darwin 11, Linux 35
    case ENAMETOOLONG:    return 36;
    case ENOLCK:          return 37;
    case ENOSYS:          return 38;   // Darwin 78 -- the one that broke clone3
    case ENOTEMPTY:       return 39;
    case ELOOP:           return 40;
    case ENOMSG:          return 42;
    case EIDRM:           return 43;
    case ENOSTR:          return 60;
    case ENODATA:         return 61;
    case ETIME:           return 62;
    case ENOSR:           return 63;
    case EREMOTE:         return 66;
    case ENOLINK:         return 67;
    case EPROTO:          return 71;
    case EMULTIHOP:       return 72;
    case EBADMSG:         return 74;
    case EOVERFLOW:       return 75;
    case EILSEQ:          return 84;
    case EUSERS:          return 87;
    case ENOTSOCK:        return 88;
    case EDESTADDRREQ:    return 89;
    case EMSGSIZE:        return 90;
    case EPROTOTYPE:      return 91;
    case ENOPROTOOPT:     return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case ENOTSUP:         return 95;   // Linux EOPNOTSUPP; Darwin aliases them
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:      return 95;   // Darwin's socket flavour, 102: it read as EIO
#endif
    case EPFNOSUPPORT:    return 96;
    case EAFNOSUPPORT:    return 97;
    case EADDRINUSE:      return 98;
    case EADDRNOTAVAIL:   return 99;
    case ENETDOWN:        return 100;
    case ENETUNREACH:     return 101;
    case ENETRESET:       return 102;
    case ECONNABORTED:    return 103;
    case ECONNRESET:      return 104;
    case ENOBUFS:         return 105;
    case EISCONN:         return 106;
    case ENOTCONN:        return 107;
    case ESHUTDOWN:       return 108;
    case ETOOMANYREFS:    return 109;
    case ETIMEDOUT:       return 110;
    case ECONNREFUSED:    return 111;
    case EHOSTDOWN:       return 112;
    case EHOSTUNREACH:    return 113;
    case EALREADY:        return 114;
    case EINPROGRESS:     return 115;
    case ESTALE:          return 116;
    case EDQUOT:          return 122;
    case ECANCELED:       return 125;
    case EOWNERDEAD:      return 130;
    case ENOTRECOVERABLE: return 131;

    default:
        // Better a plausible generic failure than a number that means something
        // unrelated on the other side.
        return 5; // Linux EIO
    }
}
