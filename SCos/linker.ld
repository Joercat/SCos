OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS {
    _kernel_start = .;

    . = 0x1000;

    .text ALIGN(4K) : {
        *(.text*)
        *(.gnu.linkonce.t*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
        *(.gnu.linkonce.r*)
    }

    .data ALIGN(4K) : {
        *(.data*)
        *(.gnu.linkonce.d*)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
        *(.gnu.linkonce.b*)
        . = ALIGN(4K);
    }

    .ctors ALIGN(4K) : {
        __CTOR_LIST__ = .;
        KEEP(*(.ctors))
        KEEP(*(SORT(.ctors.*)))
        __CTOR_END__ = .;
    }

    .dtors ALIGN(4K) : {
        __DTOR_LIST__ = .;
        KEEP(*(.dtors))
        KEEP(*(SORT(.dtors.*)))
        __DTOR_END__ = .;
    }

    _kernel_end = .;
}
