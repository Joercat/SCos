/* CPL3 app image.  A single flat load image: code, rodata, data and bss are
 * contiguous from USER_BASE, so the kernel needs no ELF program-header parser
 * and no runtime relocation pass.  Linked absolute, no PIE, no PLT. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(user_start)
SECTIONS {
    . = 0x0000600000040000;
    .text : { KEEP(*(.text.user.entry)) *(.text .text.*) }
    . = ALIGN(4096);
    _user_text_end = .;
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    _user_bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    _user_bss_end = .;
    _user_image_end = .;
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.note.GNU-stack) }
}
ASSERT(user_start == 0x0000600000040000, "user entry must be the first byte")
