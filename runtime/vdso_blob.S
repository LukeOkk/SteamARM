// The vDSO image (runtime/vdso/vdso.so, built by the Makefile), embedded.
        .section __TEXT,__const
        .p2align 4
        .globl _lxrt_vdso_image, _lxrt_vdso_image_end
_lxrt_vdso_image:
        .incbin "runtime/vdso/vdso.so"
_lxrt_vdso_image_end:
