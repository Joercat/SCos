/*
 * SCos native - About app.
 *
 * Shows the logo plus a live report of the machine the kernel is actually
 * running on: CPU brand from CPUID, RAM from owned UEFI extents, driver availability,
 * video mode from the GOP handoff, uptime from the PIT.
 */
#include "scos.h"

static void ab_paint(struct window *w)
{
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    int cx = s->w / 2;
    int phase = (int)(tick_count / 8) % 8;
    s_scos_logo(s, cx - 37, 14, t->main, 2, phase);

    int y = 66;
    char line[120];
    strcpy(line, "SCos x64 development - build " SCOS_BUILD_TAG);
    s_text(s, cx - s_text_width(line) / 2, y, line, t->main); y += 24;

    s_line(s, 24, y, s->w - 24, y, t->main); y += 14;

    char cpu[49];
    cpu_brand(cpu, sizeof(cpu));
    strcpy(line, "CPU:      "); strcat(line, cpu);
    s_clip_text(s, 28, y, line, t->text, s->w - 56); y += 20;

    u64 tot = 0, fre = 0;
    mm_stats(&tot, &fre);
    strcpy(line, "Memory:   ");
    char n[16];
    fmt_u64(n, tot / 1024); strcat(line, n); strcat(line, " MB managed, ");
    fmt_u64(n, fre / 1024); strcat(line, n); strcat(line, " MB free");
    s_text(s, 28, y, line, t->text); y += 20;

    const char *model = ata_model();
    strcpy(line, "Disk:     ");
    strcat(line, model && model[0] ? model : "no supported ATA disk");
    s_clip_text(s, 28, y, line, t->text, s->w - 56); y += 20;

    strcpy(line, "Video:    ");
    char v[12];
    fmt_u32(v, (u32)screen_w); strcat(line, v); strcat(line, "x");
    fmt_u32(v, (u32)screen_h); strcat(line, v); strcat(line, "x");
    fmt_u32(v, fb_bpp()); strcat(line, v);
    strcat(line, " GOP linear framebuffer");
    s_text(s, 28, y, line, t->text); y += 20;

    u32 up = uptime_ms() / 1000;
    strcpy(line, "Uptime:   ");
    fmt_u32(v, up / 3600); strcat(line, v); strcat(line, "h ");
    fmt_u32(v, (up / 60) % 60); strcat(line, v); strcat(line, "m ");
    fmt_u32(v, up % 60); strcat(line, v); strcat(line, "s");
    s_text(s, 28, y, line, t->text); y += 20;

    strcpy(line, "Theme:    ");
    strcat(line, t->name);
    s_text(s, 28, y, line, t->text); y += 26;

    static const char *feats[] = {
        "Native x64 UEFI loader and relocatable 64-bit kernel",
        "PS/2 keyboard + mouse drivers, PIT timer, CMOS realtime clock",
        "GOP framebuffer compositor with window manager and themes",
        "RAM filesystem; verified ATA data-partition saves",
        "Terminal, Files, Notepad, Calendar, Settings,",
        "Blackjack, System Monitor, Web Browser (stub)",
    };
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        s_text(s, 28, y, "-", t->main);
        s_clip_text(s, 40, y, feats[i], t->text, s->w - 68);
        y += 19;
    }
}

static void ab_key(struct window *w, struct key_event *e) { (void)w; (void)e; }
static void ab_mouse(struct window *w, struct mouse_event *e, int x, int y) { (void)w; (void)e; (void)x; (void)y; }

struct app app_about = {
    .id = "about", .title = "About SCos", .icon = ICON_INFO, .single = 0,
    .def_w = 520, .def_h = 460,
    .paint = ab_paint, .key = ab_key, .mouse = ab_mouse,
};
