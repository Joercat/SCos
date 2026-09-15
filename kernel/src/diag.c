/*
 * SCos native - held input/USB diagnostics screen.
 *
 * On a machine with no serial console the kernel log is invisible, and a
 * boot line that flashes for a second cannot be read or photographed. When
 * input never arrives (or USB enumeration reports a failure) this screen
 * takes over: black background, the full kernel log ring, and a countdown.
 * It stays up for 25 s so the user can photograph it, and any key or click
 * (i.e. input starting to work) dismisses it immediately.
 */
#include "scos.h"

static void diag_draw(int secs_left)
{
    const struct theme *t = theme_current();
    fb_clear(0x000000);
    s_text_scaled(&screen, 24, 14, "SCos INPUT DIAGNOSTICS", t->main, 2);
    s_text(&screen, 24, 54,
           "No input reached the OS, or USB enumeration reported a problem.",
           t->text);
    s_text(&screen, 24, 72,
           "PHOTOGRAPH THIS WHOLE SCREEN and send it - the log below names",
           t->text);
    s_text(&screen, 24, 90, "the exact stage that failed.", t->text);
    s_text(&screen, 24, 122, "kernel log (oldest first):", t->main);
    int y = 146;
    int n = klog_ring_count();
    int maxrows = (screen_h - 200) / 18;
    int start = n > maxrows ? n - maxrows : 0;
    for (int i = start; i < n; i++) {
        char ln[96];
        if (klog_ring(i, ln, sizeof(ln))) {
            s_text(&screen, 24, y, ln, t->text);
            y += 18;
        }
    }
    {
        char foot[80], num[8];
        strcpy(foot, "continues in ");
        fmt_u32(num, (u32)(secs_left > 0 ? secs_left : 0));
        strcat(foot, num);
        strcat(foot, " s  -  any key or click continues now");
        s_text(&screen, 24, screen_h - 30, foot, t->main);
    }
    fb_flip();
}

static void diag_hold(void)
{
    u64 t0 = tick_count * 10;
    int last_secs = -1;
    diag_draw(25);
    for (;;) {
        usb_poll();
        struct mouse_event me;
        struct key_event ke;
        while (mouse_poll(&me)) { }
        while (kbd_poll(&ke)) { }
        u64 now = tick_count * 10;
        int secs = 25 - (int)((now - t0) / 1000);
        if (secs <= 0) break;
        /* input arrived while we were watching: success, hand back */
        if (input_last_tick && input_last_tick >= (u32)(t0 / 10) &&
            now - t0 > 1000)
            break;
        if (secs != last_secs) {
            diag_draw(secs);
            last_secs = secs;
        }
        sleep_ms(60);
    }
}

void diag_run(void)
{
    diag_draw(25);
    diag_hold();
}

/* on-demand full system scan (Settings button / `diag` terminal command) */
void diag_manual(void)
{
    klog("diag: manual scan triggered");
    pci_scan_dump();
    char ul[96];
    usb_status(ul, sizeof(ul));
    klog("diag: %s", ul);
    klog("diag: ps/2 mouse %s", mouse_present() ? "present" : "absent");
    klog("diag: input %s", input_last_tick ? "events seen" : "silent");
    diag_draw(25);
    diag_hold();
}
