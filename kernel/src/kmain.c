/*
 * SCos native - kernel main: hardware init sequence, boot splash, handoff
 * to the window manager main loop.
 */
#include "scos.h"

struct boot_info boot_info;

void kmain(struct boot_info *bi)
{
    boot_info = *bi;

    klog("SCos native kernel starting");
    klog("  lfb %x %dx%d pitch %u, mem %u KB", bi->lfb_base, bi->width,
         bi->height, bi->pitch, bi->mem_kb);

    mm_init();
    idt_init();
    pic_remap();
    pit_init(100);
    kbd_init();
    irq_enable();          /* ticks must flow before splash animations sleep */

    fb_init();
    theme_set_index(0);
    boot_screen_init();

    boot_screen_step("Initializing system core...", 20);

    boot_screen_step("Loading file system...", 40);
    vfs_init_defaults();
    ata_init();
    fs_image_load();
    acpi_init();
    theme_load_from_settings();

    boot_screen_step("Starting window manager...", 60);
    mouse_init();
    apps_register_all();
    wm_init();

    boot_screen_step("Loading desktop environment...", 80);
    struct rtc_time rt;
    rtc_read(&rt);
    klog("rtc: %d-%d-%d %d:%d:%d", rt.year, rt.mon, rt.day, rt.hour, rt.min, rt.sec);

    boot_screen_step("Finishing... I think...", 100);
    sleep_ms(400);

    klog("boot complete, handing over to window manager");
    wm_run();

    for (;;) cpu_hlt();
}
