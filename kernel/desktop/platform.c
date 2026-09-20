/* Explicit platform boundary for the reused apps. Unsupported services report
 * unavailable; no pretend USB controller, network stack or disk writes. */
#include "scos.h"
u64 cpu_tsc_hz;
void cpu_hlt(void){
    uintptr_t flags;__asm__ volatile("pushfq;popq %0;cli":"=r"(flags)::"memory");
    if(flags&512){cpu_idle_begin();__asm__ volatile("sti;hlt":::"memory");}
    else __asm__ volatile("hlt");
}
void irq_enable(void){__asm__ volatile("sti":::"memory");}
void irq_disable(void){__asm__ volatile("cli":::"memory");}
u32 uptime_ms(void){return (u32)(timer_ticks*10);}
void sleep_ms(u32 ms){u64 end=timer_ticks+(ms+9ULL)/10;while(timer_ticks<end)cpu_hlt();}
void cpu_reboot_8042(void){
    irq_disable();
    for(unsigned i=0;i<100000;i++)if(!(inb(0x64)&2)){outb(0x64,0xfe);break;}
    for(unsigned i=0;i<100000;i++)__asm__ volatile("pause");
    panic("reset controller did not restart the machine");
}
void kernel_panic(const char *reason){panic(reason);}
int acpi_shutdown(void){klog_raw("power: ACPI shutdown is not ported; manual power-off fallback");return 0;}
int ata_present(void){return 0;} /* driver availability, not PCI disk detection */
const char *ata_model(void){return NULL;}
int fs_image_found;
int fs_image_available(void){return 0;}
int fs_image_save(void){return 0;}
void input_status(char *out,int max){
    if(max<=0)return;
    const char *s=mouse_present()?"PS/2 keyboard/mouse; USB HID driver not ported":"PS/2 keyboard; USB HID driver not ported";
    strncpy(out,s,(size_t)max-1);out[max-1]=0;
}
void desktop_start(const struct boot_framebuffer *fb){
    /* Apps share one foreground execution context, so SSE2 is caller-saved by
     * SysV. IRQ/NMI/panic code is compiled general-register-only and never calls
     * app callbacks. A future preemptive scheduler MUST save FP/SIMD state. */
    uintptr_t cr0,cr4;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    cr0=(cr0&~12ULL)|2;cr4|=(1ULL<<9)|(1ULL<<10);
    __asm__ volatile("mov %0,%%cr0;mov %1,%%cr4;fninit"::"r"(cr0),"r"(cr4):"memory");
    u32 mxcsr=0x1f80;__asm__ volatile("ldmxcsr %0"::"m"(mxcsr));
    mm_init();desktop_framebuffer(fb);fb_init();
    if(!vfs_init_defaults())panic("initial desktop filesystem allocation failed");
    system_files_init(0);theme_load_from_settings();apps_register_all();
    kbd_init();mouse_init();cpu_meter_init();wm_init();
    klog_raw("desktop: original SCos apps active in x86-64; files/settings are RAM-only");
    for(;;){wm_run();tty_run(0);}
}
const char *fs_image_target(void){return "unavailable: persistence driver not ported";}
