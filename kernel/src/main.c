#include "kernel.h"
static struct boot_handoff boot;
static struct boot_map_entry memory_map[BOOT_MAP_MAX];
/* No dynamic/module driver loader existed in i386. This explicit dependency
 * order starts only core platform facilities; legacy devices are NOT called. */
void kernel_main(const struct boot_handoff *incoming) {
    console_init();
    putstr("SCos x86-64 conversion - startup foundation (unnumbered)\n");
    interrupts_init();
    putstr("64-bit GDT/TSS/IDT installed; dedicated fault stacks ready\n");
    if((uintptr_t)incoming!=0x7000) panic("unexpected boot handoff pointer");
    memcpy(&boot,incoming,sizeof(boot));
    if(boot.magic!=BOOT_MAGIC || boot.version!=BOOT_VERSION || boot.size!=sizeof(boot) ||
       boot.map_address!=0x5000 || !boot.map_count || boot.map_count>BOOT_MAP_MAX ||
       boot.map_stride!=sizeof(memory_map[0]) || boot.flags!=1 || boot.reserved ||
       boot.bios_drive>255 || boot.bootstrap_cr3!=0x10000 || read_cr3()!=boot.bootstrap_cr3 ||
       boot.kernel_start!=(uintptr_t)_kernel_start || boot.kernel_end!=(uintptr_t)_kernel_end ||
       boot.kernel_file_end!=(uintptr_t)_file_end || boot.kernel_file_end>boot.kernel_end ||
       boot.kernel_end>BOOT_IDENTITY_LIMIT)
        panic("invalid or incompatible boot handoff");
    memcpy(memory_map,(const void*)(uintptr_t)boot.map_address,boot.map_count*sizeof(memory_map[0]));
    if(!boot_map_valid(memory_map,boot.map_count)) panic("invalid E820 map");
    uint32_t lo,hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));
    (void)hi;
    uint64_t cr0,cr4;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    if((lo&0xd00)!=0xd00 || !(cr4&32) || (cr0&0x80010001)!=0x80010001)
        panic("long-mode/paging protection state invalid");
    putstr("Long mode active; 8-byte pointers; EFER=");puthex(lo);putstr("\n");
    putstr("Validated E820 entries: ");puthex(boot.map_count);putstr("\n");
    for(size_t i=0;i<boot.map_count;i++) if(memory_map[i].type==1 && (memory_map[i].attributes&1) &&
                                               memory_map[i].length && memory_map[i].base>=UINT64_C(0x100000000)) {
        putstr("Usable RAM above 4 GiB (not allocated yet): ");puthex(memory_map[i].base);putstr("\n");
    }
    memory_init(&boot,memory_map);
    putstr("Kernel page tables active: ");puthex(read_cr3());putstr("\n");
    putstr("W^X/NX active; null page and stack guards unmapped\n");
    putstr("Early usable 4-KiB pages below 16 MiB: ");puthex(memory_free_pages());putstr("\n");
    /* Validate allocator state transitions without enabling an unconverted heap. */
    uint64_t before=memory_free_pages();
    uintptr_t page=page_allocate();
    if(!page || memory_free_pages()!=before-1) panic("early allocator failed");
    *(volatile uint64_t*)page=UINT64_C(0x53434f533634);
    if(*(volatile uint64_t*)page!=UINT64_C(0x53434f533634)) panic("RAM verification failed");
    page_release(page);
    if(memory_free_pages()!=before) panic("page accounting failed");
    timer_init();
    putstr("Core platform init complete. Waiting for real PIT interrupts...\n");
    __asm__ volatile("sti");
    while(timer_ticks<3) __asm__ volatile("hlt");
    putstr("PIT interrupts and 64-bit interrupt return verified\n");
    putstr("Foundation ready. Desktop/device ports are not enabled yet.\n");
    putstr("This development image does not read or write saved user data.\n");
    for(;;) __asm__ volatile("hlt");
}
