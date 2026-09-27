/* Linux aarch64 vDSO layout: one text image, the data page 16 KiB below it. */
SECTIONS
{
    PROVIDE(__vvar = . - 0x4000);
    . = SIZEOF_HEADERS;
    .hash          : { *(.hash) }            :text
    .gnu.hash      : { *(.gnu.hash) }
    .dynsym        : { *(.dynsym) }
    .dynstr        : { *(.dynstr) }
    .gnu.version   : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }
    .dynamic       : { *(.dynamic) }         :text :dynamic
    .got           : { *(.got) *(.got.plt) } :text
    .rodata        : { *(.rodata*) }         :text
    .text          : { *(.text*) }           :text
    /DISCARD/      : { *(.data*) *(.bss*) *(.comment) *(.note*) *(.eh_frame*) }
}
PHDRS
{
    text    PT_LOAD FLAGS(5) FILEHDR PHDRS;
    dynamic PT_DYNAMIC FLAGS(4);
}
VERSION
{
    LINUX_2.6.39 {
    global:
        __kernel_clock_gettime;
        __kernel_gettimeofday;
        __kernel_clock_getres;
    local: *;
    };
}
