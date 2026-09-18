/* SCos native kernel linker script: flat binary loaded at 1 MB */
ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    _kernel_start = .;

    /* must start exactly at 0x100000: the flat binary is loaded there */
    .text :
    {
        *(.text)
        *(.text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(4K);
    _kernel_end = .;
}
