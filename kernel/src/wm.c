/*
 * SCos native - window manager / compositor / desktop shell.
 *
 * Draws the wallpaper, desktop icons, decorated windows (drag, resize,
 * minimize, maximize, close, focus), context menus, modal dialogs, the
 * taskbar with running apps + clock, and the mouse cursor. All interaction
 * is mouse- and keyboard-driven through the PS/2 drivers.
 */
#include "scos.h"

#define MAX_WINDOWS 48
#define TASKBAR_H 40
#define BTN_SZ 18

struct window wins[MAX_WINDOWS];
static int win_count;
static int next_z = 10, next_id = 1;
static struct window *focused_w;
static struct window *modal_w;
static int dirty = 1;

static int mx = 300, my = 300;
static u8 mbuttons;
static struct window *drag_win, *resize_win;
static int drag_ox, drag_oy;

static struct {
    int active, x, y, w, h;
    const char **items;
    int n;
    menu_cb cb;
    void *ud;
} menu;

/* easter egg: home/documents/file.scv spawns ERROR popups (as in web sim) */
static int egg_armed;
static u64 egg_last_spawn;

static u64 last_blink_phase;

static const struct { const char *app; int icon; const char *label; } desk_icons[] = {
    { "files",    ICON_FOLDER,   "Files"    },
    { "terminal", ICON_TERMINAL, "Terminal" },
    { "notepad",  ICON_NOTEPAD,  "Notepad"  },
    { "browser",  ICON_BROWSER,  "Browser"  },
    { "calendar", ICON_CALENDAR, "Calendar" },
    { "settings", ICON_SETTINGS, "Settings" },
    { "about",    ICON_INFO,     "About"    },
};
#define N_ICONS ((int)(sizeof(desk_icons)/sizeof(desk_icons[0])))
static int icon_hover = -1;

/* ------------------------------------------------------------- utils ---- */
static u32 blend(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (((ar * (255 - t) + br * t) / 255) << 16) |
           (((ag * (255 - t) + bg * t) / 255) << 8) |
           (((ab * (255 - t) + bb * t) / 255));
}

static int in_rect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

int wm_win_count(void) { return win_count; }
struct window *wm_win_at(int i) { return &wins[i]; }
struct window *wm_focused(void) { return focused_w; }
int wm_dialog_active(void) { return modal_w != NULL; }

int wm_content_w(struct window *w) { return w->w - 2; }
int wm_content_h(struct window *w) { return w->h - WIN_TITLEBAR - 1; }

void wm_redraw(struct window *w) { (void)w; dirty = 1; }

static void win_alloc_buf(struct window *w)
{
    int cw = wm_content_w(w), ch = wm_content_h(w);
    if (cw < 8) cw = 8;
    if (ch < 8) ch = 8;
    w->surf.px = palloc(cw * ch * 4);
    w->surf.w = cw;
    w->surf.h = ch;
    memset(w->surf.px, 0, cw * ch * 4);
}

static void win_free_buf(struct window *w)
{
    if (w->surf.px) {
        pfree(w->surf.px, w->surf.w * w->surf.h * 4);
        w->surf.px = NULL;
    }
}

void wm_set_title(struct window *w, const char *title)
{
    strncpy(w->title, title, sizeof(w->title) - 1);
    dirty = 1;
}

void wm_focus(struct window *w)
{
    if (!w) return;
    w->z = ++next_z;
    focused_w = w;
    dirty = 1;
}

static struct window *win_at_point(int x, int y)
{
    struct window *best = NULL;
    for (int i = 0; i < win_count; i++) {
        struct window *w = &wins[i];
        if (w->state == WIN_STATE_MIN) continue;
        if (in_rect(x, y, w->x, w->y, w->w, w->h))
            if (!best || w->z > best->z) best = w;
    }
    return best;
}

void wm_close_window(struct window *w)
{
    if (!w || w->closing) return;
    w->closing = 1;
    if (w->app && w->app->close) w->app->close(w);
    win_free_buf(w);
    int idx = -1;
    for (int i = 0; i < win_count; i++) if (&wins[i] == w) idx = i;
    if (idx >= 0) {
        for (int i = idx; i < win_count - 1; i++) wins[i] = wins[i + 1];
        win_count--;
    }
    if (focused_w == w) focused_w = NULL;
    if (modal_w == w) modal_w = NULL;
    if (drag_win == w) drag_win = NULL;
    if (resize_win == w) resize_win = NULL;
    if (!focused_w) {
        struct window *top = NULL;
        for (int i = 0; i < win_count; i++)
            if (!top || wins[i].z > top->z) top = &wins[i];
        focused_w = top;
    }
    dirty = 1;
}

struct window *wm_open_app(const char *app_id, void *arg)
{
    struct app *app = app_find(app_id);
    if (!app) return NULL;
    if (app->single)
        for (int i = 0; i < win_count; i++)
            if (wins[i].app == app) {
                if (wins[i].state == WIN_STATE_MIN) wins[i].state = WIN_STATE_NORMAL;
                wm_focus(&wins[i]);
                return &wins[i];
            }
    if (win_count >= MAX_WINDOWS) return NULL;
    struct window *w = &wins[win_count++];
    memset(w, 0, sizeof(*w));
    w->id = next_id++;
    w->app = app;
    strncpy(w->title, app->title, sizeof(w->title) - 1);
    int dw = app->def_w ? app->def_w : 600;
    int dh = app->def_h ? app->def_h : 400;
    if (dw > screen_w - 8) dw = screen_w - 8;
    if (dh > screen_h - TASKBAR_H - 8) dh = screen_h - TASKBAR_H - 8;
    w->w = dw; w->h = dh;
    w->x = 40 + (win_count * 28) % (screen_w - dw - 60 > 0 ? screen_w - dw - 60 : 1);
    w->y = 24 + (win_count * 26) % (screen_h - TASKBAR_H - dh - 40 > 0 ? screen_h - TASKBAR_H - dh - 40 : 1);
    w->z = ++next_z;
    w->state = WIN_STATE_NORMAL;
    win_alloc_buf(w);
    if (app->open) app->open(w, arg);
    focused_w = w;
    dirty = 1;
    return w;
}

void wm_init(void)
{
    win_count = 0;
    focused_w = NULL;
    modal_w = NULL;
    mx = screen_w / 2;
    my = screen_h / 2;
    dirty = 1;
}

/* -------------------------------------------------------------- menus ---- */
void wm_menu(int x, int y, const char **items, int n, menu_cb cb, void *ud)
{
    menu.active = 1;
    menu.items = items;
    menu.n = n;
    menu.cb = cb;
    menu.ud = ud;
    menu.w = 170;
    menu.h = n * 24 + 6;
    menu.x = x; menu.y = y;
    if (menu.x + menu.w > screen_w) menu.x = screen_w - menu.w - 2;
    if (menu.y + menu.h > screen_h) menu.y = screen_h - menu.h - 2;
    dirty = 1;
}

/* ------------------------------------------------------------- paint ---- */
static void paint_wallpaper(void)
{
    const struct theme *t = theme_current();
    s_vgrad(&screen, 0, 0, screen_w, screen_h, t->bg_top, t->bg_bot);
    u32 grid = blend(t->bg_top, t->main, 14);
    for (int x = 0; x < screen_w; x += 64)
        s_fill(&screen, x, 0, 1, screen_h, grid);
    for (int y = 0; y < screen_h; y += 64)
        s_fill(&screen, 0, y, screen_w, 1, grid);
    /* subtle corner glow */
    s_disc(&screen, screen_w - 80, 90, 60, blend(t->bg_top, t->main, 8));
}

static void icon_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = 16 + (i % 8) * 88;
    *y = 16 + (i / 8) * 96;
    *w = 80; *h = 88;
}

static void paint_icons(void)
{
    const struct theme *t = theme_current();
    icon_hover = -1;
    for (int i = 0; i < N_ICONS; i++) {
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);
        if (in_rect(mx, my, x, y, w, h)) {
            icon_hover = i;
            s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 18));
        }
        s_icon(&screen, desk_icons[i].icon, x + (w - 24) / 2, y + 8, t->main);
        int tw = s_text_width(desk_icons[i].label);
        s_text(&screen, x + (w - tw) / 2, y + 40, desk_icons[i].label, t->text);
    }
}

static void draw_win_button(struct surface *s, int x, int y, int kind, u32 fg)
{
    /* kind 0 minimize, 1 maximize, 2 close */
    if (kind == 0) s_fill(s, x + 4, y + 12, 10, 2, fg);
    else if (kind == 1) s_frame_rect(s, x + 4, y + 4, 10, 10, fg);
    else {
        s_line(s, x + 4, y + 4, x + 13, y + 13, fg);
        s_line(s, x + 13, y + 4, x + 4, y + 13, fg);
    }
}

static void paint_window(struct window *w)
{
    const struct theme *t = theme_current();
    if (w->state == WIN_STATE_MIN) return;
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    /* shadow */
    s_fill(&screen, x + 4, y + 4, ww, hh, blend(t->bg_bot, 0x000000, 60));
    /* frame + title bar */
    u32 title_bg = (focused_w == w) ? t->main : blend(t->main, t->win_bg, 55);
    s_fill(&screen, x, y, ww, hh, t->win_bg);
    s_frame_rect(&screen, x, y, ww, hh, t->main);
    s_fill(&screen, x + 1, y + 1, ww - 2, WIN_TITLEBAR - 2, title_bg);
    u32 title_fg = (focused_w == w) ? t->title_text : t->text;
    s_clip_text(&screen, x + 8, y + 5, w->title, title_fg, ww - 90);
    for (int b = 0; b < 3; b++)
        draw_win_button(&screen, x + ww - 22 - (2 - b) * 22, y + 3, b, title_fg);

    /* content */
    s_blit(&screen, &w->surf, x + 1, y + WIN_TITLEBAR);

    /* resize handle */
    u32 hz = blend(t->win_bg, t->main, 60);
    for (int i = 0; i < 4; i++)
        s_fill(&screen, x + ww - 12 + i * 3, y + hh - 12 + i * 3, 2, 2, hz);
}

static void paint_menu(void)
{
    if (!menu.active) return;
    const struct theme *t = theme_current();
    s_fill(&screen, menu.x, menu.y, menu.w, menu.h, t->win_bg);
    s_frame_rect(&screen, menu.x, menu.y, menu.w, menu.h, t->main);
    for (int i = 0; i < menu.n; i++) {
        int iy = menu.y + 3 + i * 24;
        if (in_rect(mx, my, menu.x, iy, menu.w, 24))
            s_fill(&screen, menu.x + 1, iy, menu.w - 2, 24, blend(t->win_bg, t->main, 25));
        s_text(&screen, menu.x + 10, iy + 4, menu.items[i], t->text);
    }
}

static void paint_taskbar(void)
{
    const struct theme *t = theme_current();
    int y = screen_h - TASKBAR_H;
    s_fill(&screen, 0, y, screen_w, TASKBAR_H, t->taskbar_bg);
    s_fill(&screen, 0, y, screen_w, 1, t->main);

    int bx = 8;
    for (int i = 0; i < win_count && bx < screen_w - 140; i++) {
        struct window *w = &wins[i];
        int bw = 124;
        u32 bg = (w == focused_w && w->state != WIN_STATE_MIN)
                 ? blend(t->taskbar_bg, t->main, 35)
                 : blend(t->taskbar_bg, t->main, 12);
        s_fill(&screen, bx, y + 6, bw, TASKBAR_H - 12, bg);
        s_frame_rect(&screen, bx, y + 6, bw, TASKBAR_H - 12, blend(t->taskbar_bg, t->main, 50));
        s_clip_text(&screen, bx + 6, y + 12, w->title, t->text, bw - 12);
        bx += bw + 6;
    }

    struct rtc_time rt;
    rtc_read(&rt);
    char clock[16];
    char a[4], b[4], c[4];
    fmt_pad2(a, rt.hour); fmt_pad2(b, rt.min); fmt_pad2(c, rt.sec);
    strcpy(clock, a); strcat(clock, ":"); strcat(clock, b); strcat(clock, ":"); strcat(clock, c);
    int tw = s_text_width(clock);
    s_text(&screen, screen_w - tw - 12, y + 12, clock, t->main);
}

static void paint_cursor(void)
{
    static const u8 shape[12] = { 0x70, 0x78, 0x7C, 0x7E, 0x7F, 0x7F,
                                  0x77, 0x73, 0x71, 0x60, 0x40, 0x00 };
    for (int off = 1; off >= 0; off--)
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 7; x++)
                if (shape[y] & (0x80 >> x))
                    s_pixel(&screen, mx + x + off, my + y + off, off ? 0x000000 : 0xFFFFFF);
}

static void paint_all(void)
{
    paint_wallpaper();
    paint_icons();
    /* windows in z order (bottom first) */
    static u8 drawn[MAX_WINDOWS];
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    for (int pass = 0; pass < win_count; pass++) {
        struct window *lowest = NULL;
        for (int i = 0; i < win_count; i++) {
            if (drawn[i] || wins[i].state == WIN_STATE_MIN) continue;
            if (!lowest || wins[i].z < lowest->z) lowest = &wins[i];
        }
        if (!lowest) break;
        drawn[lowest - wins] = 1;
        if (lowest->app && lowest->app->paint) lowest->app->paint(lowest);
        paint_window(lowest);
    }
    paint_menu();
    paint_taskbar();
    paint_cursor();
    fb_flip();
}

/* -------------------------------------------------------------- input ---- */
static void handle_mouse(struct mouse_event *e)
{
    if (e->type == MEV_MOVE) {
        mx += e->dx; my += e->dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > screen_w - 1) mx = screen_w - 1;
        if (my > screen_h - 1) my = screen_h - 1;
        if (drag_win) {
            drag_win->x = mx - drag_ox;
            drag_win->y = my - drag_oy;
            if (drag_win->y < 0) drag_win->y = 0;
            if (drag_win->y > screen_h - TASKBAR_H - WIN_TITLEBAR)
                drag_win->y = screen_h - TASKBAR_H - WIN_TITLEBAR;
        }
        if (resize_win) {
            struct window *w = resize_win;
            int nw = mx - w->x, nh = my - w->y;
            if (nw < 320) nw = 320;
            if (nh < 200) nh = 200;
            if (nw > screen_w) nw = screen_w;
            if (nh > screen_h - TASKBAR_H) nh = screen_h - TASKBAR_H;
            if (nw != w->w || nh != w->h) {
                int cw = nw - 2, ch = nh - WIN_TITLEBAR - 1;
                u32 *np = palloc((u32)cw * ch * 4);
                if (np) {
                    pfree(w->surf.px, (u32)w->surf.w * w->surf.h * 4);
                    w->w = nw; w->h = nh;
                    w->surf.px = np; w->surf.w = cw; w->surf.h = ch;
                    memset(np, 0, (u32)cw * ch * 4);
                    if (w->app && w->app->paint) w->app->paint(w);
                }
            }
        }
        dirty = 1;
        return;
    }
    if (e->type == MEV_WHEEL) {
        struct window *w = win_at_point(mx, my);
        if (w && w->app && w->app->mouse)
            w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR));
        dirty = 1;
        return;
    }
    /* buttons */
    mbuttons = e->buttons;
    if (!e->down) {
        drag_win = NULL;
        resize_win = NULL;
        dirty = 1;
        return;
    }

    if (menu.active) {
        if (in_rect(mx, my, menu.x, menu.y, menu.w, menu.h)) {
            int item = (my - menu.y - 3) / 24;
            menu_cb cb = menu.cb;
            void *ud = menu.ud;
            menu.active = 0;
            if (cb && item >= 0 && item < menu.n) cb(item, ud);
        } else {
            menu.active = 0;
        }
        dirty = 1;
        return;
    }

    struct window *w = win_at_point(mx, my);
    if (w) {
        wm_focus(w);
        int rx = mx - w->x, ry = my - w->y;
        /* title buttons */
        if (ry < WIN_TITLEBAR) {
            for (int b = 0; b < 3; b++) {
                int bx = w->w - 22 - (2 - b) * 22;
                if (in_rect(rx, ry, bx, 3, BTN_SZ, BTN_SZ)) {
                    if (b == 0) { w->state = WIN_STATE_MIN; }
                    else if (b == 1) {
                        if (w->state == WIN_STATE_MAX) {
                            win_free_buf(w);
                            w->x = w->px; w->y = w->py; w->w = w->pw; w->h = w->ph;
                            w->state = WIN_STATE_NORMAL;
                            win_alloc_buf(w);
                        } else {
                            w->px = w->x; w->py = w->y; w->pw = w->w; w->ph = w->h;
                            win_free_buf(w);
                            w->x = 0; w->y = 0;
                            w->w = screen_w; w->h = screen_h - TASKBAR_H;
                            w->state = WIN_STATE_MAX;
                            win_alloc_buf(w);
                        }
                    } else {
                        wm_close_window(w);
                    }
                    dirty = 1;
                    return;
                }
            }
            drag_win = w;
            drag_ox = rx; drag_oy = ry;
            dirty = 1;
            return;
        }
        /* resize handle */
        if (in_rect(rx, ry, w->w - 16, w->h - 16, 16, 16) && w->state != WIN_STATE_MAX) {
            resize_win = w;
            dirty = 1;
            return;
        }
        if (w->app && w->app->mouse)
            w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR));
        dirty = 1;
        return;
    }

    /* desktop icons */
    for (int i = 0; i < N_ICONS; i++) {
        int x, y, ww, hh;
        icon_rect(i, &x, &y, &ww, &hh);
        if (in_rect(mx, my, x, y, ww, hh)) {
            wm_open_app(desk_icons[i].app, NULL);
            dirty = 1;
            return;
        }
    }
    /* taskbar */
    if (my >= screen_h - TASKBAR_H) {
        int bx = 8;
        for (int i = 0; i < win_count; i++) {
            int bw = 124;
            if (in_rect(mx, my, bx, screen_h - TASKBAR_H + 6, bw, TASKBAR_H - 12)) {
                struct window *tw = &wins[i];
                if (tw->state == WIN_STATE_MIN) {
                    tw->state = WIN_STATE_NORMAL;
                    wm_focus(tw);
                } else if (tw == focused_w) {
                    tw->state = WIN_STATE_MIN;
                } else {
                    wm_focus(tw);
                }
                dirty = 1;
                return;
            }
            bx += bw + 6;
        }
    }
    dirty = 1;
}

static void handle_key(struct key_event *e)
{
    if (menu.active && e->pressed && e->keycode == 27) {
        menu.active = 0;
        dirty = 1;
        return;
    }
    struct window *w = modal_w ? modal_w : focused_w;
    if (w && w->app && w->app->key) w->app->key(w, e);
    dirty = 1;
}

/* --------------------------------------------------------------- tick ---- */
static void wm_tick(void)
{
    for (int i = 0; i < win_count; i++)
        if (wins[i].app && wins[i].app->tick && wins[i].state != WIN_STATE_MIN)
            wins[i].app->tick(&wins[i]);

    /* clock / blink phase */
    u64 phase = tick_count / 50;
    if (phase != last_blink_phase) {
        last_blink_phase = phase;
        dirty = 1;
    }

    /* easter egg, mirrors the web simulation's file.scv behaviour */
    if (tick_count > 500) {
        if (!egg_armed) { egg_armed = 1; egg_last_spawn = tick_count; }
        if ((tick_count % 50) == 0 && vfs_lookup("home/documents/file.scv")) {
            if (tick_count - egg_last_spawn >= 30 && win_count < MAX_WINDOWS - 2) {
                egg_last_spawn = tick_count;
                wm_error_popup("A critical error has occurred!");
            }
        }
    }
}

/* ----------------------------------------------------------- main loop --- */
/* If the timer interrupt ever stops arriving while wall-clock time keeps
 * advancing (lost EOI / stuck PIC on some real hardware), re-arm the PIC and
 * PIT so the system recovers instead of hanging. */
static void irq_watchdog(void)
{
    static u32 last_sec, last_tick;
    static int checks;
    if (++checks < 200000) return;
    checks = 0;
    struct rtc_time rt;
    rtc_read(&rt);
    if (last_sec && rt.sec != last_sec && (u32)tick_count == last_tick) {
        outb(0x20, 0x20);
        outb(0xA0, 0x20);
        pic_clear_mask(0);
        klog("wm: irq watchdog re-armed PIC");
    }
    last_sec = rt.sec;
    last_tick = (u32)tick_count;
}

void wm_run(void)
{
    klog("wm: entering main loop");
    for (;;) {
        irq_watchdog();
        struct mouse_event me;
        while (mouse_poll(&me)) handle_mouse(&me);
        struct key_event ke;
        while (kbd_poll(&ke)) handle_key(&ke);

        static u64 last_tick;
        if (tick_count != last_tick) {
            last_tick = tick_count;
            wm_tick();
        }
        if (dirty) {
            paint_all();
            dirty = 0;
        }
        cpu_hlt();
    }
}

/* ------------------------------------------------------------- dialogs --- */
struct dialog_data {
    char title[48];
    char message[256];
    char input[128];
    int  has_input;
    int  pos;
    int  resolved;
    dialog_cb cb;
    void *ud;
};

#define DLG_OK_X 40
#define DLG_CANCEL_X 130
#define DLG_BTN_Y_OFF 24

static void dlg_paint(struct window *w)
{
    struct dialog_data *d = w->data;
    const struct theme *t = theme_current();
    struct surface *s = &w->surf;
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);
    s_text(s, 14, 14, d->message, t->text);
    int y = 14 + 20;
    for (char *p = d->message; *p; p++) if (*p == '\n') y += 20;
    if (d->has_input) {
        s_frame_rect(s, 14, y, s->w - 28, 24, t->main);
        s_clip_text(s, 18, y + 4, d->input, t->text, s->w - 40);
        int cx = 18 + d->pos * FONT_W;
        if ((tick_count / 50) % 2 == 0)
            s_fill(s, cx, y + 4, 2, 16, t->main);
        y += 32;
    }
    y = s->h - 40;
    s_fill(s, DLG_OK_X, y, 70, 26, blend(t->win_bg, t->main, 25));
    s_frame_rect(s, DLG_OK_X, y, 70, 26, t->main);
    s_text(s, DLG_OK_X + 22, y + 5, "OK", t->text);
    s_fill(s, DLG_CANCEL_X, y, 80, 26, blend(t->win_bg, t->main, 12));
    s_frame_rect(s, DLG_CANCEL_X, y, 80, 26, t->main);
    s_text(s, DLG_CANCEL_X + 16, y + 5, "Cancel", t->text);
}

static void dlg_resolve(struct window *w, int ok)
{
    struct dialog_data *d = w->data;
    d->resolved = 1;
    dialog_cb cb = d->cb;
    void *ud = d->ud;
    char *text = (ok && d->has_input) ? d->input : NULL;
    modal_w = NULL;
    if (cb) cb(ok, text, ud);
    wm_close_window(w);
}

static void dlg_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    if (e->type != MEV_BUTTON || !e->down || e->button != MBTN_LEFT) return;
    int by = w->surf.h - 40;
    if (in_rect(x, y, DLG_OK_X, by, 70, 26)) dlg_resolve(w, 1);
    else if (in_rect(x, y, DLG_CANCEL_X, by, 80, 26)) dlg_resolve(w, 0);
}

static void dlg_key(struct window *w, struct key_event *e)
{
    struct dialog_data *d = w->data;
    if (d->has_input) {
        if (e->pressed && e->keycode == '\n') { dlg_resolve(w, 1); return; }
        if (e->pressed && e->keycode == 27)   { dlg_resolve(w, 0); return; }
        edit_line(d->input, &d->pos, sizeof(d->input), e);
        return;
    }
    if (e->pressed && (e->keycode == '\n' || e->keycode == 27))
        dlg_resolve(w, e->keycode == '\n');
}

static void dlg_close(struct window *w)
{
    struct dialog_data *d = w->data;
    if (d && !d->resolved && d->cb) d->cb(0, NULL, d->ud);
    if (d) pfree(d, sizeof(*d));
    w->data = NULL;
}

static struct app dialog_app = {
    .id = "_dialog", .title = "Dialog", .icon = ICON_INFO, .single = 0,
    .def_w = 420, .def_h = 200,
    .paint = dlg_paint, .key = dlg_key, .mouse = dlg_mouse, .close = dlg_close,
};

void wm_dialog(const char *title, const char *message, const char *input,
               dialog_cb cb, void *ud)
{
    struct window *w = wm_open_app("_dialog", NULL);
    if (!w) { if (cb) cb(0, NULL, ud); return; }
    struct dialog_data *d = palloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    strncpy(d->title, title, sizeof(d->title) - 1);
    strncpy(d->message, message, sizeof(d->message) - 1);
    if (input) {
        d->has_input = 1;
        strncpy(d->input, input, sizeof(d->input) - 1);
        d->pos = (int)strlen(d->input);
    }
    d->cb = cb; d->ud = ud;
    w->data = d;
    wm_set_title(w, title);
    modal_w = w;
    wm_focus(w);
}

/* -------------------------------------------------------- error popups --- */
struct err_data { char text[128]; };

static void err_paint(struct window *w)
{
    struct surface *s = &w->surf;
    s_fill(s, 0, 0, s->w, s->h, 0x1a1a1a);
    s_text(s, 14, 14, w->data ? ((struct err_data *)w->data)->text : "Error", 0xFF3333);
}

static void err_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct err_data));
    w->data = NULL;
}

static struct app error_app = {
    .id = "_error", .title = "ERROR", .icon = ICON_INFO, .single = 0,
    .def_w = 300, .def_h = 120,
    .paint = err_paint, .close = err_close,
};

void wm_error_popup(const char *text)
{
    struct window *w = wm_open_app("_error", NULL);
    if (!w) return;
    struct err_data *d = palloc(sizeof(*d));
    strncpy(d->text, text, sizeof(d->text) - 1);
    w->data = d;
    /* error windows appear near-randomly, like the web sim */
    w->x = 60 + (int)(tick_count * 7 % 400);
    w->y = 40 + (int)(tick_count * 5 % 250);
}

void wm_fatal_screen(const char *line1, const char *line2)
{
    if (!screen.px) { for (;;) cpu_hlt(); }
    fb_clear(0x000080);
    s_text_scaled(&screen, 40, 60, line1, 0xFFFFFF, 3);
    s_text(&screen, 40, 140, line2, 0xFFFFFF);
    s_text(&screen, 40, 170, "The system has been halted. Power off manually.", 0xFFFFFF);
    fb_flip();
}

/* app registry lives in apps.c; expose dialog/error apps to it */
struct app *wm_dialog_app(void) { return &dialog_app; }
struct app *wm_error_app(void) { return &error_app; }

void wm_poweroff_screen(void)
{
    fb_clear(0x000000);
    const struct theme *t = theme_current();
    s_text_scaled(&screen, (screen_w - 4 * 8 * 3) / 2, screen_h / 2 - 60, "SCos", t->main, 3);
    const char *m = "It is now safe to turn off your computer.";
    s_text(&screen, (screen_w - s_text_width(m)) / 2, screen_h / 2, m, 0xAAAAAA);
    fb_flip();
}
