OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS {
    . = 0x1000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .ctors : {
        __CTOR_LIST__ = .;
        *(.ctors)
        *(.init_array)
        __CTOR_END__ = .;
    }

    .dtors : {
        __DTOR_LIST__ = .;
        *(.dtors)
        *(.fini_array)
        __DTOR_END__ = .;
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }

    _kernel_end = .;
}