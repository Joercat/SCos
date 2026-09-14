/*
 * SCos native - Browser app stub.
 *
 * The web simulation embedded an <iframe>. A real browser inside the kernel
 * would need a NIC driver, a full TCP/IP stack, TLS, and an HTML/CSS layout
 * engine - well over 10k lines - so, per project scope, it is skipped.
 * This window explains that honestly instead of pretending.
 */
#include "scos.h"

static void br_open(struct window *w, void *arg) { (void)arg; (void)w; }

static void br_paint(struct window *w)
{
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);
    s_frame_rect(s, 8, 8, s->w - 16, 26, t->main);
    s_text(s, 14, 14, "https://(no network stack)", ((t->main >> 1) & 0x7F7F7F));

    static const char *lines[] = {
        "Web Browser is not available in SCos native.",
        "",
        "The original simulation just embedded the host browser.",
        "A real kernel-side browser would require:",
        "  - a network card driver (e1000/rtl8139/...)",
        "  - a full TCP/IP stack with DNS",
        "  - TLS 1.3 + certificate validation",
        "  - an HTML/CSS/JS rendering engine",
        "Together far beyond this project's scope (10k+ lines),",
        "so this app was intentionally skipped.",
        "",
        "Everything else from the simulation is implemented natively.",
    };
    int y = 50;
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        s_text(s, 16, y, lines[i], i == 0 ? t->main : t->text);
        y += 20;
    }
}

static void br_key(struct window *w, struct key_event *e) { (void)w; (void)e; }
static void br_mouse(struct window *w, struct mouse_event *e, int x, int y) { (void)w; (void)e; (void)x; (void)y; }

struct app app_browser = {
    .id = "browser", .title = "Web Browser", .icon = ICON_BROWSER, .single = 0,
    .def_w = 640, .def_h = 420,
    .open = br_open, .paint = br_paint, .key = br_key, .mouse = br_mouse,
};
