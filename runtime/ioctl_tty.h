// ioctl(2) translation. The _IOC encoding, the request numbers and the
// structures behind them all differ between Linux and Darwin; a raw pass-through
// runs a random Darwin ioctl, exactly the svc problem in miniature.
//
// Covered: the terminal family (TCGETS/TCSETS*/TCGETS2/TCSETS2, TIOCGWINSZ,
// TIOCSWINSZ, TIOCGPGRP/TIOCSPGRP, TIOCGSID, TIOCSCTTY, TIOCNOTTY, TIOCOUTQ,
// TCFLSH, TIOCEXCL/TIOCNXCL) and the fd-generic FIONREAD/FIONBIO/FIOCLEX/
// FIONCLEX. Anything else returns ENOTTY, which is what Linux itself returns
// for a request the descriptor's driver does not know, and is traced.
#ifndef LXRT_IOCTL_TTY_H
#define LXRT_IOCTL_TTY_H
#include <stdint.h>
long lxrt_ioctl(int fd, unsigned long lreq, uint64_t arg);
void lxrt_pty_slave_opened(const char *host_path);   // the guest opened /dev/ttysNNN
void lxrt_pty_close(int fd);                          // a descriptor closed

#endif
