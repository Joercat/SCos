/* SCos native - About app */
#include "scos.h"

static void ab_open(struct window *w, void *arg) { (void)arg; (void)w; }

static void ab_paint(struct window *w)
{
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    int cx = s->w / 2;
    /* logo with spinning 'o' (animated ring of dots) */
    int phase = (int)(tick_count / 8) % 8;
    s_text_scaled(s, cx - 4 * 8 * 2, 20, "SC", t->main, 2);
    s_text_scaled(s, cx + 0 * 8 * 2, 20, "s", t->main, 2);
    int ox = cx - 8 * 2 + 8, oy = 20 + 16;
    for (int k = 0; k < 8; k++) {
        int a = (k + phase) % 8;
        static const int dxs[8] = { 0, 5, 7, 5, 0, -5, -7, -5 };
        static const int dys[8] = { -7, -5, 0, 5, 7, 5, 0, -5 };
        int on = (a < 3);
        s_disc(s, ox + dxs[k], oy + dys[k], on ? 2 : 1, on ? t->main : ((t->main >> 2) & 0x3F3F3F));
    }

    int y = 70;
    char line[96];
    strcpy(line, "Version: 2.0.0 (native kernel)");
    s_text(s, cx - s_text_width(line) / 2, y, line, t->text); y += 22;
    strcpy(line, "Released: September 2026");
    s_text(s, cx - s_text_width(line) / 2, y, line, t->text); y += 22;
    strcpy(line, "SCos is a simulated computer operating system.");
    s_text(s, cx - s_text_width(line) / 2, y, line, t->text); y += 20;
    strcpy(line, "This build runs as real bare-metal x86 code.");
    s_text(s, cx - s_text_width(line) / 2, y, line, t->text); y += 30;

    strcpy(line, "Features:");
    s_text(s, 40, y, line, t->text); y += 22;
    static const char *feats[] = {
        "- Fully functional in-memory file system (+ optional disk persistence)",
        "- Window management with drag, resize, and minimize",
        "- Terminal with 20+ commands",
        "- Text editor",
        "- Calendar application",
        "- Customizable themes",
        "- PS/2 mouse and keyboard support",
    };
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        s_text(s, 56, y, feats[i], t->text);
        y += 20;
    }
}

static void ab_key(struct window *w, struct key_event *e) { (void)w; (void)e; }
static void ab_mouse(struct window *w, struct mouse_event *e, int x, int y) { (void)w; (void)e; (void)x; (void)y; }

struct app app_about = {
    .id = "about", .title = "About SCos", .icon = ICON_INFO, .single = 0,
    .def_w = 520, .def_h = 460,
    .open = ab_open, .paint = ab_paint, .key = ab_key, .mouse = ab_mouse,
};
