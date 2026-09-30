// A stand-in for the vendor's libavservices_minijail.so (i386), given to
// Android's 32-bit OMX store alone (scripts/android-boot.py, "vendor32").
// Its SetUpMinijail installs the service's seccomp filter; under FEX's
// seccomp emulation the 32-bit filter crashed FEX in its own code and the
// store spun at 100% CPU instead of registering (MEASURED, stage 29), and
// without the emulation the filter cannot be installed and the service
// aborts. These no-ops leave the store without that filter: a seccomp
// filter inside a process that is an ordinary Mac process of this user
// isolates nothing from macOS. No VM (AGENTS.md).
//
// Built by android-boot.py with Homebrew clang for i686-linux-android30.
__attribute__((visibility("default")))
void _ZN7android13SetUpMinijailERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_(const void *base, const void *additional)
{
    (void)base; (void)additional;
}

__attribute__((visibility("default")))
void _ZN7android17SetUpMinijailListERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKNS0_6vectorIS6_NS4_IS6_EEEE(const void *base, const void *additional)
{
    (void)base; (void)additional;
}

__attribute__((visibility("default")))
int _ZN7android17WritePolicyToPipeERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKNS0_6vectorIS6_NS4_IS6_EEEE(const void *base, const void *additional)
{
    (void)base; (void)additional;
    return -1;
}
