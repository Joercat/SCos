/* Native startup/order boundary for the reused apps and original drivers.
 * Unsupported controllers retain explicit unavailable/fallback behavior. */
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
void input_status(char *out,int max){usb_status(out,max);}
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
    boot_screen_init();
    boot_screen_step("core: native IDT, PIT interrupts and page allocator ready",10);
    char line[128],number[24],cpu[49];
    cpu_brand(cpu,sizeof(cpu));strcpy(line,"cpu: ");strncat(line,cpu,48);
    boot_screen_step(line,18);
    strcpy(line,"mem: ");fmt_u64(number,mm_total_kb());strcat(line,number);strcat(line," KB managed");
    boot_screen_step(line,25);
    strcpy(line,"video: GOP ");fmt_u32(number,fb->width);strcat(line,number);strcat(line,"x");fmt_u32(number,fb->height);strcat(line,number);
    boot_screen_step(line,32);graphics_init(fb);
    if(!vfs_init_defaults())panic("initial desktop filesystem allocation failed");
    boot_screen_step("vfs: factory file tree built",40);
    acpi_init();int drives=ata_init();
    strcpy(line,"ata: ");fmt_u32(number,(u32)drives);strcat(line,number);strcat(line," drive(s); ");strncat(line,fs_image_target(),70);
    boot_screen_step(line,52);
    int loaded=fs_image_load();system_files_init(fs_image_available());
    boot_screen_step(loaded?"fs: verified saved tree loaded":fs_image_available()?"fs: defaults active; verified save target available":"fs: RAM-only; no supported verified save target",62);
    kbd_init();mouse_init();usb_init();
    input_status(line,sizeof(line));boot_screen_step(line,75);
    struct rtc_time rtc;rtc_read(&rtc);
    strcpy(line,"rtc: ");fmt_u32(number,rtc.year);strcat(line,number);strcat(line,"-");fmt_pad2(number,rtc.mon);strcat(line,number);strcat(line,"-");fmt_pad2(number,rtc.day);strcat(line,number);
    boot_screen_step(line,82);
    theme_load_from_settings();apps_register_all();cpu_meter_init();wm_init();
    wm_notify("GPU acceleration unavailable","Using CPU software rendering. No compatible GPU backend is linked. Run graphics in Terminal for detected hardware.",1);
    boot_screen_step("wm: compositor ready; native and Lua apps registered",94);
    boot_screen_step("Finishing... I think...",100);
    /* Original readable finished log; service input while showing it. */
    u64 until=timer_ticks+140;
    while(timer_ticks<until){usb_poll_events();cpu_hlt();}
    klog_raw("desktop: SCos native/Lua apps active in x86-64; native PCI/USB/ATA/ACPI services initialized");
    for(;;){wm_run();tty_run(0);}
}
