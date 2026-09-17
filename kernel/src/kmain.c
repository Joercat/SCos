/*
 * SCos native - kernel main: hardware init sequence, boot splash, handoff
 * to the window manager main loop.
 */
#include "scos.h"

struct boot_info boot_info;

void kmain(struct boot_info *bi)
{
    boot_info = *bi;

    klog("SCos kernel starting - build r33");
    klog("  lfb %x %dx%d pitch %u, mem %u KB", bi->lfb_base, bi->width,
         bi->height, bi->pitch, bi->mem_kb);

    mm_init();
    idt_init();
    pic_remap();
    pit_init(100);
    kbd_init();
    irq_enable();          /* ticks must flow before splash animations sleep */
    cpu_meter_init();      /* needs sleeping, so only after interrupts are on */

    fb_init();
    theme_set_index(0);
    boot_screen_init();

    {
        char line[128], n[16];
        strcpy(line, "core: IDT + PIT @ 100 Hz + PS/2 keyboard online");
        boot_screen_step(line, 15);
        char cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        strcpy(line, "cpu:  ");
        strncat(line, cpu[0] ? cpu : "x86 processor", 60);
        strcat(line, " @ "); fmt_u32(n, cpu_mhz()); strcat(line, n); strcat(line, " MHz");
        boot_screen_step(line, 25);
        strcpy(line, "mem:  ");
        fmt_u32(n, boot_info.mem_kb); strcat(line, n); strcat(line, " KB probed, page allocator ready");
        boot_screen_step(line, 35);
        strcpy(line, "video: VBE ");
        fmt_u32(n, (u32)boot_info.width); strcat(line, n); strcat(line, "x");
        fmt_u32(n, (u32)boot_info.height); strcat(line, n); strcat(line, "x");
        fmt_u32(n, boot_info.bpp); strcat(line, n); strcat(line, " framebuffer");
        boot_screen_step(line, 45);
        vfs_init_defaults();
        boot_screen_step("vfs:  factory file tree built", 55);
        int drives = ata_init();
        const char *model = ata_model();
        strcpy(line, "ata:  ");
        fmt_u32(n, (u32)drives); strcat(line, n); strcat(line, " drive(s)");
        if (model && model[0]) { strcat(line, " - "); strncat(line, model, 40); }
        boot_screen_step(line, 65);
        boot_screen_step(fs_image_load() ? "fs:   saved image loaded from disk (LBA 2048)"
                                         : "fs:   no saved image on disk - using defaults", 75);
        system_files_init(drives > 0);
    if (drives > 0)
        boot_screen_step("sys:  /system boot-chain files read from disk", 78);
    mouse_init();
        usb_init();
        {
            char ul[160];
            usb_status(ul, sizeof(ul));
            strcpy(line, "in:   ps2 kbd");
            if (mouse_present()) strcat(line, "+mouse");
            strcat(line, " | ");
            strncat(line, ul, 70);
            boot_screen_step(line, 80);
        }
        boot_screen_step("wm:   compositor starting, apps registered", 85);
        apps_register_all();
        wm_init();
        struct rtc_time rt2;
        rtc_read(&rt2);
        strcpy(line, "rtc:  ");
        fmt_u32(n, (u32)rt2.year); strcat(line, n); strcat(line, "-");
        fmt_pad2(n, rt2.mon); strcat(line, n); strcat(line, "-");
        fmt_pad2(n, rt2.day); strcat(line, n);
        strcat(line, " "); fmt_pad2(n, rt2.hour); strcat(line, n); strcat(line, ":");
        fmt_pad2(n, rt2.min); strcat(line, n); strcat(line, ":");
        fmt_pad2(n, rt2.sec); strcat(line, n);
        boot_screen_step(line, 95);
        boot_screen_step("Finishing... I think...", 100);
        sleep_ms(1400);   /* let the finished log be read (and photographed) */
    }

    klog("boot complete, handing over to window manager");
    wm_run();

    for (;;) cpu_hlt();
}
