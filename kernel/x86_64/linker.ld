OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x100000;
    _kernel_start = .;
    _text_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } :text
    . = ALIGN(4096);
    _text_end = .;
    _rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4096);
    _rodata_end = .;
    .data : { *(.data .data.*) } :data
    _file_end = .;
    . = ALIGN(4096);
    _bss_start = .;
    .bss (NOLOAD) : { *(.bss.stacks) *(.bss .bss.*) *(COMMON) } :data
    _bss_end = .;
    . = ALIGN(4096);
    _kernel_end = .;
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(_start == 0x100000, "entry must be first byte of flat image")
ASSERT(_kernel_end <= 0x1000000, "bootstrap kernel exceeds mapped memory")
