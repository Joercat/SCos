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
    s_text_scaled(&screen, 24, 10, "SCos INPUT DIAGNOSTICS  -  build r31", t->main, 2);
    s_text(&screen, 24, 42,
           "PHOTOGRAPH THIS WHOLE SCREEN and send it - the log names the failing stage.",
           t->text);
    s_text(&screen, 24, 58, "kernel log (oldest first, newest at bottom):",
           t->main);
    int y = 74;
    int n = klog_ring_count();
    int maxrows = (screen_h - 104) / 11;
    int start = n > maxrows ? n - maxrows : 0;
    for (int i = start; i < n; i++) {
        char ln[128];
        if (klog_ring(i, ln, sizeof(ln))) {
            s_text(&screen, 24, y, ln, t->text);
            y += 11;
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
    /* the click/key that OPENED this screen already stamped input_last_tick;
     * only a genuinely NEW event (stamp changes while we hold) counts as
     * "input started working". Comparing against the entry stamp also stops
     * the old re-show loop where the opening click dismissed the screen a
     * second later and the button could fire again. */
    u32 base = input_last_tick;
    int last_secs = -1;
    diag_draw(25);
    for (;;) {
        usb_poll();
        struct mouse_event me;
        struct key_event ke;
        while (mouse_poll(&me)) { }
        while (kbd_poll(&ke)) { }
        u64 now = tick_count * 10;
        int secs = 30 - (int)((now - t0) / 1000);
        if (secs <= 0) break;
        /* input arrived while we were watching: success, hand back */
        if (input_last_tick != base && now - t0 > 1000)
            break;
        if (secs != last_secs) {
            diag_draw(secs);
            last_secs = secs;
        }
        sleep_ms(60);
    }
    /* the countdown screen is stale now - make the WM repaint everything,
     * or its next partial paint (taskbar clock) draws over this screen */
    wm_request_full();
}

void diag_run(void)
{
    diag_draw(30);
    diag_hold();
}

/* on-demand full system scan (Settings button / `diag` terminal command) */
void diag_manual(void)
{
    klog("diag: manual scan triggered");
    pci_scan_dump();
    char ul[160];
    usb_status(ul, sizeof(ul));
    klog("diag: %s", ul);
    klog("diag: ps/2 mouse %s", mouse_present() ? "present" : "absent");
    klog("diag: input %s", input_last_tick ? "events seen" : "silent");
    {   /* injection self-test: HID report -> scancode/queue -> WM poll path,
         * no hardware involved; events are consumed here so nothing leaks */
        u8 prevk[6] = {0}, prevm = 0;
        u8 rep[6] = {0x04, 0, 0, 0, 0, 0};      /* HID usage 0x04 = 'a' */
        kbd_inject_hid(0, rep, prevk, &prevm);
        u8 rel[6] = {0, 0, 0, 0, 0, 0};
        kbd_inject_hid(0, rel, prevk, &prevm);
        int kok = 0;
        struct key_event ke;
        while (kbd_poll(&ke))
            if (ke.pressed && ke.keycode == 'a') kok = 1;
        mouse_inject(0, 40, -12, 0);
        int mok = 0;
        struct mouse_event me;
        while (mouse_poll(&me))
            if (me.type == MEV_MOVE && me.dx == 40 && me.dy == -12) mok = 1;
        klog("diag: self-test kbd injection %s, mouse injection %s",
             kok ? "OK" : "FAILED", mok ? "OK" : "FAILED");
    }
    diag_draw(30);
    diag_hold();
}
