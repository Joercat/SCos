/*
 * SCos native - boot splash screen: logo with spinning 'o', progress bar and
 * boot messages, mirroring the web simulation's loading screen (but driven
 * by real initialization steps).
 */
#include "scos.h"

static int msg_line;
static int logo_x, logo_y, bar_x, bar_y, msg_x, msg_y;

static void draw_logo(int phase)
{
    const struct theme *t = theme_current();
    s_scos_logo(&screen, logo_x, logo_y, t->main, 3, phase);
}

void boot_screen_init(void)
{
    const struct theme *t = theme_current();
    fb_clear(0x000000);
    logo_x = (screen_w - 102) / 2;
    logo_y = 40;
    bar_x = (screen_w - 300) / 2;
    bar_y = logo_y + 90;
    msg_x = (screen_w - 560) / 2;
    msg_y = bar_y + 40;
    s_frame_rect(&screen, bar_x, bar_y, 300, 20, t->main);
    s_text(&screen, msg_x, msg_y - 22, "SCos 2.0 - native kernel boot log", t->text);
    draw_logo(0);
    fb_flip();
    msg_line = 0;
}

void boot_screen_step(const char *msg, int progress_pct)
{
    const struct theme *t = theme_current();
    /* every line is produced by the real init step that just completed */
    s_text(&screen, msg_x, msg_y + msg_line * 18, "[ OK ]", t->main);
    s_text(&screen, msg_x + 56, msg_y + msg_line * 18, msg, t->text);
    msg_line++;
    s_fill(&screen, bar_x + 2, bar_y + 2, (296 * progress_pct) / 100, 16, t->main);
    if (progress_pct >= 100) {
        /* the one ceremonial step: brief spinner, like the old simulation */
        for (int f = 0; f < 24; f++) {
            draw_logo(f / 3);
            fb_flip();
            sleep_ms(16);
        }
    } else {
        fb_flip();
    }
}
