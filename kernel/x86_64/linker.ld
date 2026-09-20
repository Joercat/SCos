OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
    . = 0;
    _kernel_start = .;
    _text_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } :text
    . = ALIGN(4096);
    _text_end = .;
    _rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela*) } :rodata
    . = ALIGN(4096);
    _rodata_end = .;
    _relro_start = .;
    .data.rel.ro : { *(.data.rel.ro*) } :data
    . = ALIGN(4096);
    _relro_end = .;
    .dynamic : { *(.dynamic) } :data :dynamic
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
ASSERT(_start == 0, "PIE entry offset")
ASSERT(_kernel_end <= 0x4000000, "kernel span exceeds UEFI policy")
ASSERT((stack_guard % 4096) == 0 && (df_guard % 4096) == 0 &&
       (nmi_guard % 4096) == 0 && (mc_guard % 4096) == 0,
       "stack guards must be page aligned")
