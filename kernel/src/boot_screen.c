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
    s_text_scaled(&screen, logo_x, logo_y, "SC", t->main, 3);
    s_text_scaled(&screen, logo_x + 6 * 8 * 3, logo_y, "s", t->main, 3);
    /* spinning 'o': a ring of dots with a bright arc rotating */
    int ox = logo_x + 2 * 8 * 3 + 12, oy = logo_y + 24;
    static const int dxs[8] = { 0, 6, 9, 6, 0, -6, -9, -6 };
    static const int dys[8] = { -9, -6, 0, 6, 9, 6, 0, -6 };
    for (int k = 0; k < 8; k++) {
        int rel = (k - phase + 16) % 8;
        int on = rel < 3;
        s_disc(&screen, ox + dxs[k], oy + dys[k], on ? 3 : 2,
               on ? t->main : ((t->main >> 2) & 0x3F3F3F));
    }
}

void boot_screen_init(void)
{
    const struct theme *t = theme_current();
    fb_clear(0x000000);
    logo_x = (screen_w - 3 * 8 * 3) / 2;
    logo_y = screen_h / 2 - 140;
    bar_x = (screen_w - 300) / 2;
    bar_y = logo_y + 90;
    msg_x = (screen_w - 400) / 2;
    msg_y = bar_y + 40;
    s_frame_rect(&screen, bar_x, bar_y, 300, 20, t->main);
    draw_logo(0);
    fb_flip();
    msg_line = 0;
}

void boot_screen_step(const char *msg, int progress_pct)
{
    const struct theme *t = theme_current();
    /* message line */
    s_text(&screen, msg_x, msg_y + msg_line * 20, msg, t->main);
    msg_line++;
    /* animate: spinner + progress for a short while */
    for (int f = 0; f < 24; f++) {
        draw_logo(f / 3);
        s_fill(&screen, bar_x + 2, bar_y + 2, (296 * progress_pct) / 100, 16, t->main);
        fb_flip();
        sleep_ms(16);
    }
}
