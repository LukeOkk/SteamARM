// SPDX-License-Identifier: MIT
#pragma once

// W^X for FEX running as a Linux ELF on top of the SteamARM Darwin runtime.
//
// FEX maps its code buffers PROT_READ|PROT_WRITE|PROT_EXEC. On a Linux kernel
// that is exactly what it gets. Under the runtime the mapping is turned into
// Darwin MAP_JIT, where a thread may write the page or execute it but never
// both, and the switch is per-thread rather than per-page.
//
// The runtime cannot infer the switch from the faults: a
// pthread_jit_write_protect_np() issued inside a signal handler does not
// survive the handler's return -- measured, benchmarks/stage5-jit.txt. So FEX
// has to ask, on its own stack.
//
// Private syscall 0x4C580020 in x8, (enable, addr, len):
//    0  open this thread for writing
//    1  close it for execution, after scanning [addr,len) for instructions the
//       runtime cannot let run (the JIT emits `svc`, which on Darwin runs
//       whatever Darwin syscall happens to be in x16)
//    2  scan [addr,len) but stay writable -- an inner scope, where an outer one
//       still owns the mode
//
// Nothing about this is macOS-specific at compile time, and it must not be:
// this binary is built by a Linux toolchain. A real Linux kernel has no such
// syscall number, returns -ENOSYS, the probe latches false once, and every
// scope below costs one predictable branch. One binary, both hosts.

#include <cstddef>
#include <cstdint>

namespace FEXCore::LxrtJit {

#if defined(__aarch64__) && defined(__linux__)

inline long Call(long Enable, uint64_t Addr, uint64_t Len) {
  register long x8 asm("x8") = 0x4C580020;
  register long x0 asm("x0") = Enable;
  register uint64_t x1 asm("x1") = Addr;
  register uint64_t x2 asm("x2") = Len;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
  return x0;
}

// Asking for execute mode is the safe probe: under the runtime it is the state
// a freshly mapped JIT region is already in, and on Linux it does nothing.
inline bool Available() {
  static const bool Supported = Call(1, 0, 0) == 0;
  return Supported;
}

// RAII. Construct around every write into an executable buffer; pass the range
// that write lands in so the runtime scans that and nothing else.
class WriteScope final {
public:
  explicit WriteScope(void* Addr = nullptr, size_t Len = 0)
    : Base(reinterpret_cast<uint64_t>(Addr)), Size(Len) {
    if (!Available()) {
      return;
    }
    Active = true;
    Owner = (Depth++ == 0);
    if (Owner) {
      Call(0, 0, 0);
    }
  }

  ~WriteScope() {
    if (!Active) {
      return;
    }
    --Depth;
    // An inner scope must not close write mode, but its range still has to be
    // scanned while the memory is writable -- hence enable=2.
    Call(Owner ? 1 : 2, Base, Size);
  }

  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;

  static int GetDepth() { return Depth; }

private:
  uint64_t Base;
  size_t Size;
  bool Active = false;
  bool Owner = false;
  inline static thread_local int Depth = 0;
};

#else

class WriteScope final {
public:
  explicit WriteScope(void* = nullptr, size_t = 0) {}
  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;
  static int GetDepth() { return 0; }
};
inline bool Available() { return false; }

#endif

} // namespace FEXCore::LxrtJit
