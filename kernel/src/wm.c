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
static void desktop_load(void);
static void desktop_save(void);
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
    { "blackjack", ICON_CARDS,   "Blackjack" },
    { "sysmon",   ICON_CHART,    "SysMon"   },
    { "solitaire", ICON_SOL,     "Solitaire" },
};
#define N_ICONS ((int)(sizeof(desk_icons)/sizeof(desk_icons[0])))
static int icon_hover = -1;

/* ---- taskbar layout: scrollable app buttons + power button + clock ---- */
#define TB_BTN_W   124
#define TB_BTN_GAP 6
#define TB_CLOCK_W 68
#define TB_POWER_W 34
static int tb_off = 0;            /* index of first visible task button */
static int power_hover = 0;

static void tb_geom(int *x0, int *x1, int *vis, int *scrollable)
{
    *x0 = 8 + 40;                        /* after the launcher button */
    *x1 = screen_w - TB_CLOCK_W - TB_POWER_W - 24;
    int cap = (*x1 - *x0) / (TB_BTN_W + TB_BTN_GAP);
    if (cap < 1) cap = 1;
    *scrollable = win_count > cap;
    if (*scrollable) { *x0 += 12; *x1 -= 12; cap = (*x1 - *x0) / (TB_BTN_W + TB_BTN_GAP); if (cap < 1) cap = 1; }
    *vis = cap;
    if (tb_off > win_count - cap) tb_off = win_count - cap;
    if (tb_off < 0) tb_off = 0;
}

static void tb_power_rect(int *x, int *y, int *w, int *h)
{
    *x = screen_w - TB_CLOCK_W - TB_POWER_W - 16;
    *y = screen_h - TASKBAR_H + 6;
    *w = TB_POWER_W;
    *h = TASKBAR_H - 12;
}

static void power_menu_cb(int item, void *ud)
{
    (void)ud;
    sleep_ms(400);
    if (item == 0) {                       /* Restart */
        cpu_reboot_8042();
        for (;;) cpu_hlt();
    } else {                               /* Power Off */
        acpi_shutdown();                   /* may return without ACPI */
        wm_poweroff_screen();
        for (;;) cpu_hlt();
    }
}

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

void wm_redraw(struct window *w) { if (w) w->dirty = 1; dirty = 1; }

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
    w->dirty = 1;
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
    desktop_load();
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
static struct surface wp_cache;
static int wp_valid;

void wm_wallpaper_invalidate(void) { wp_valid = 0; }

/*
 * The wallpaper is static per theme: render it once into a cache and blit
 * from there on every composite (per-pixel gradient + discs used to run on
 * every single repaint, which dominated CPU use in slow emulators).
 */
static void paint_wallpaper(void)
{
    const struct theme *t = theme_current();
    if (!wp_valid) {
        if (!wp_cache.px) {
            wp_cache.px = palloc((u32)screen_w * screen_h * 4);
            wp_cache.w = screen_w;
            wp_cache.h = screen_h;
        }
        if (wp_cache.px) {
            struct surface *c = &wp_cache;
            s_vgrad(c, 0, 0, screen_w, screen_h, t->bg_top, t->bg_bot);
            u32 grid = blend(t->bg_top, t->main, 14);
            for (int x = 0; x < screen_w; x += 64)
                s_fill(c, x, 0, 1, screen_h, grid);
            for (int y = 0; y < screen_h; y += 64)
                s_fill(c, 0, y, screen_w, 1, grid);
            /* soft corner glow: stacked discs, intensity falls off outward */
            for (int r = 100, pc = 10; r >= 20; r -= 10, pc--)
                s_disc(c, screen_w - 80, 90, r, blend(t->bg_top, t->main, pc));
            wp_valid = 1;
        }
    }
    if (wp_valid)
        memcpy(screen.px, wp_cache.px, (u32)screen_w * screen_h * 4);
    else
        s_fill(&screen, 0, 0, screen_w, screen_h, t->bg_top);
}

/* ---- desktop items: app launchers + pinned files on a snap grid ---- */
#define DESK_MAX  32
#define DESK_COLS 11
#define DESK_ROWS 7
struct ditem { int kind; char app[32]; char path[192]; char label[40];
               int gx, gy, hidden; };
static struct ditem items[DESK_MAX];
static int nitems;
static u32 desk_sel;                       /* selection bitmask */
static int desk_drag = -1, desk_drag_ox, desk_drag_oy, desk_drag_moved;
static int desk_sx, desk_sy;
static int band_active, band_x0, band_y0, band_x1, band_y1;
static struct { int active; char q[48]; int qpos; int hover; } launch;

static int ditem_icon(struct ditem *d)
{
    if (d->kind == 0) { struct app *a = app_find(d->app); if (a) return a->icon; }
    return ICON_NOTEPAD;
}
static int cell_taken(int gx, int gy, int skip)
{
    for (int i = 0; i < nitems; i++)
        if (i != skip && !items[i].hidden && items[i].gx == gx && items[i].gy == gy) return 1;
    return 0;
}
static void desktop_save(void)
{
    char buf[2048];
    strcpy(buf, "SCOSDESK1\n");
    for (int i = 0; i < nitems; i++) {
        char n[8];
        fmt_u32(n, (u32)items[i].kind); strcat(buf, n); strcat(buf, "|");
        strcat(buf, items[i].app); strcat(buf, "|");
        strcat(buf, items[i].path); strcat(buf, "|");
        strcat(buf, items[i].label); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].gx); strcat(buf, n); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].gy); strcat(buf, n); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].hidden); strcat(buf, n); strcat(buf, "\n");
        if (strlen(buf) > 1900) break;
    }
    vfs_write("system/desktop.json", buf, (u32)strlen(buf));
}
static void desktop_load(void)
{
    nitems = 0; desk_sel = 0;
    u32 len = 0;
    char *data = vfs_read("system/desktop.json", &len);
    if (data && len > 10 && !memcmp(data, "SCOSDESK1", 9)) {
        char *p = data + 10;
        while (*p && nitems < DESK_MAX) {
            char *fld[7]; int nf = 0;
            fld[nf++] = p;
            while (*p && *p != '\n') { if (*p == '|') { *p = 0; if (nf < 7) fld[nf++] = p + 1; } p++; }
            if (*p) *p++ = 0;
            if (nf < 7) break;
            struct ditem *d = &items[nitems];
            d->kind = (int)str_to_u32(fld[0]);
            strncpy(d->app, fld[1], 31); d->app[31] = 0;
            strncpy(d->path, fld[2], 191); d->path[191] = 0;
            strncpy(d->label, fld[3], 39); d->label[39] = 0;
            d->gx = (int)str_to_u32(fld[4]); d->gy = (int)str_to_u32(fld[5]);
            d->hidden = (int)str_to_u32(fld[6]);
            nitems++;
        }
    }
    if (!nitems) {
        for (int i = 0; i < N_ICONS && nitems < DESK_MAX; i++) {
            struct ditem *d = &items[nitems++];
            d->kind = 0;
            strncpy(d->app, desk_icons[i].app, 31);
            d->path[0] = 0;
            strncpy(d->label, desk_icons[i].label, 39);
            d->gx = i % 8; d->gy = i / 8; d->hidden = 0;
        }
    }
}
static void desktop_open(struct ditem *d)
{
    if (d->kind == 0) wm_open_app(d->app, NULL);
    else wm_open_app("notepad", d->path);
}
static void desk_remove_sel(void)
{
    for (int i = 0; i < nitems; i++) {
        if (!(desk_sel & (1u << i))) continue;
        if (items[i].kind == 0) items[i].hidden = 1;
        else {
            for (int k = i; k + 1 < nitems; k++) items[k] = items[k + 1];
            nitems--; i--;
        }
    }
    desk_sel = 0;
    desktop_save();
}
void wm_desktop_pin_file(const char *path)
{
    if (nitems >= DESK_MAX) return;
    for (int i = 0; i < nitems; i++)
        if (!items[i].kind && !strcmp(items[i].path, path) == 0 && items[i].kind == 1 && !strcmp(items[i].path, path)) return;
    struct ditem *d = &items[nitems];
    d->kind = 1;
    d->app[0] = 0;
    strncpy(d->path, path, 191); d->path[191] = 0;
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    strncpy(d->label, base, 39); d->label[39] = 0;
    int placed = 0;
    for (int gy = 0; gy < DESK_ROWS && !placed; gy++)
        for (int gx = 0; gx < DESK_COLS && !placed; gx++)
            if (!cell_taken(gx, gy, -1)) { d->gx = gx; d->gy = gy; placed = 1; }
    if (!placed) { d->gx = 0; d->gy = 0; }
    d->hidden = 0;
    nitems++;
    desktop_save();
    dirty = 1;
}
static void desk_item_menu_cb(int item, void *ud)
{
    (void)ud;
    if (item == 0) {
        for (int i = 0; i < nitems; i++)
            if (desk_sel & (1u << i)) { desktop_open(&items[i]); break; }
    } else desk_remove_sel();
}
/* bring back every hidden app icon and re-add missing registered apps */
static void desk_restore(void)
{
    for (int i = 0; i < nitems; i++)
        if (items[i].kind == 0) items[i].hidden = 0;
    for (int a = 0; a < app_count(); a++) {
        const struct app *ap = app_at(a);
        int found = 0;
        for (int i = 0; i < nitems; i++)
            if (items[i].kind == 0 && strcmp(items[i].app, ap->id) == 0) found = 1;
        if (found || nitems >= DESK_MAX) continue;
        struct ditem *d = &items[nitems];
        memset(d, 0, sizeof(*d));
        d->kind = 0;
        strcpy(d->app, ap->id);
        strcpy(d->label, ap->title);
        int placed = 0;
        for (int gy = 0; gy < DESK_ROWS && !placed; gy++)
            for (int gx = 0; gx < DESK_COLS && !placed; gx++)
                if (!cell_taken(gx, gy, -1)) { d->gx = gx; d->gy = gy; placed = 1; }
        if (placed) nitems++;
    }
    desktop_save();
    dirty = 1;
}

void wm_desktop_restore(void) { desk_restore(); }

/* enumerate visible desktop items for the file manager's desktop view */
int wm_desk_vis_count(void)
{
    int n = 0;
    for (int i = 0; i < nitems; i++)
        if (!items[i].hidden) n++;
    return n;
}

int wm_desk_vis_get(int idx, char *app, char *path, char *label, int *kind)
{
    int n = 0;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        if (n++ != idx) continue;
        strcpy(app, items[i].app);
        strcpy(path, items[i].path);
        strcpy(label, items[i].label);
        *kind = items[i].kind;
        return 1;
    }
    return 0;
}

static void desk_empty_menu_cb(int item, void *ud)
{
    (void)item; (void)ud;
    desk_restore();
}

static void desk_sel_menu_cb(int item, void *ud)
{
    (void)ud;
    if (item == 0) desk_remove_sel();
    else desk_sel = 0;
}

static void icon_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = 16 + items[i].gx * 88;
    *y = 16 + items[i].gy * 96;
    *w = 80; *h = 88;
}

static void paint_icons(void)
{
    const struct theme *t = theme_current();
    icon_hover = -1;
    /* no hover feedback while dragging/resizing/modally busy: the pointer is
     * occupied and highlighting launch buttons underneath is misleading */
    int interactive = !drag_win && !resize_win && !menu.active && !modal_w;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);
        if (desk_drag == i && desk_drag_moved) { x = mx - desk_drag_ox; y = my - desk_drag_oy; }
        if (desk_sel & (1u << i)) {
            s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 30));
            s_frame_rect(&screen, x, y, w, h, t->main);
        } else if (interactive && in_rect(mx, my, x, y, w, h)) {
            icon_hover = i;
            s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 18));
        }
        s_icon(&screen, ditem_icon(&items[i]), x + (w - 24) / 2, y + 8, t->main);
        int tw = s_text_width(items[i].label);
        s_clip_text(&screen, x + (w - tw) / 2, y + 40, items[i].label, t->text, w);
    }
    if (band_active) {
        int x = band_x0 < band_x1 ? band_x0 : band_x1;
        int y = band_y0 < band_y1 ? band_y0 : band_y1;
        int w = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
        int h = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
        s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 15));
        s_frame_rect(&screen, x, y, w, h, t->main);
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

    /* launcher button */
    u32 lbg = launch.active ? blend(t->taskbar_bg, t->main, 45) : blend(t->taskbar_bg, t->main, 12);
    s_fill(&screen, 8, y + 6, 34, TASKBAR_H - 12, lbg);
    s_frame_rect(&screen, 8, y + 6, 34, TASKBAR_H - 12, blend(t->taskbar_bg, t->main, 60));
    u32 lc = launch.active ? t->title_text : t->main;
    for (int q = 0; q < 4; q++)
        s_fill(&screen, 15 + (q % 2) * 11, y + 12 + (q / 2) * 9, 8, 6, lc);

    int x0, x1, vis, scrollable;
    tb_geom(&x0, &x1, &vis, &scrollable);
    if (scrollable) {
        u32 ac = blend(t->taskbar_bg, t->main, 60);
        for (int k = 0; k < 3; k++) {        /* left/right scroll arrows */
            int ay = y + 14 + k * 4;
            if (tb_off > 0)
                s_fill(&screen, 8 + k, ay, 1, 8 - k * 2, ac);
            if (tb_off + vis < win_count)
                s_fill(&screen, x1 + 12 - 1 - k, ay, 1, 8 - k * 2, ac);
        }
    }
    int bx = x0;
    for (int i = tb_off; i < win_count && bx + TB_BTN_W <= x1; i++) {
        struct window *w = &wins[i];
        u32 bg = (w == focused_w && w->state != WIN_STATE_MIN)
                 ? blend(t->taskbar_bg, t->main, 35)
                 : blend(t->taskbar_bg, t->main, 12);
        s_fill(&screen, bx, y + 6, TB_BTN_W, TASKBAR_H - 12, bg);
        s_frame_rect(&screen, bx, y + 6, TB_BTN_W, TASKBAR_H - 12, blend(t->taskbar_bg, t->main, 50));
        s_clip_text(&screen, bx + 6, y + 12, w->title, t->text, TB_BTN_W - 12);
        bx += TB_BTN_W + TB_BTN_GAP;
    }

    /* power button */
    int px, py, pw, ph;
    tb_power_rect(&px, &py, &pw, &ph);
    power_hover = in_rect(mx, my, px, py, pw, ph) && !drag_win && !resize_win && !menu.active && !modal_w;
    s_fill(&screen, px, py, pw, ph, power_hover ? blend(t->taskbar_bg, t->main, 45) : blend(t->taskbar_bg, t->main, 12));
    s_frame_rect(&screen, px, py, pw, ph, blend(t->taskbar_bg, t->main, 60));
    u32 pc = power_hover ? t->title_text : t->main;
    s_circle(&screen, px + pw / 2, py + ph / 2 + 2, 7, pc);
    s_fill(&screen, px + pw / 2 - 1, py + ph / 2 - 8, 3, 9, power_hover ? t->title_text : t->taskbar_bg);
    s_line(&screen, px + pw / 2, py + ph / 2 - 8, px + pw / 2, py + ph / 2 - 1, pc);

    struct rtc_time rt;
    rtc_read(&rt);
    char clock[16];
    char a[4], b[4], c[4];
    fmt_pad2(a, rt.hour); fmt_pad2(b, rt.min); fmt_pad2(c, rt.sec);
    strcpy(clock, a); strcat(clock, ":"); strcat(clock, b); strcat(clock, ":"); strcat(clock, c);
    int tw = s_text_width(clock);
    s_text(&screen, screen_w - tw - 10, y + 12, clock, t->main);
}

/* classic arrow pointer: white fill with black outline, hotspot at tip */
static const float cur_poly[7][2] = {
    { 0, 0 }, { 0, 14 }, { 3.5f, 10.5f }, { 6, 16.5f },
    { 8.5f, 15.5f }, { 6, 9.5f }, { 10, 9.5f },
};
static void paint_cursor(void)
{
    const int n = 7;
    for (int y = 0; y < 17; y++) {
        float xs[8];
        int nx = 0;
        for (int i = 0; i < n; i++) {
            float y0 = cur_poly[i][1], y1 = cur_poly[(i + 1) % n][1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                float x0 = cur_poly[i][0], x1 = cur_poly[(i + 1) % n][0];
                xs[nx++] = x0 + (y + 0.5f - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (int i = 0; i + 1 < nx; i++)
            for (int j = i + 1; j < nx; j++)
                if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
        for (int i = 0; i + 1 < nx; i += 2)
            for (int x = (int)xs[i]; x <= (int)xs[i + 1]; x++)
                s_pixel(&screen, mx + x, my + y, 0xFFFFFF);
    }
    for (int i = 0; i < n; i++) {
        int x0 = mx + (int)cur_poly[i][0], y0 = my + (int)cur_poly[i][1];
        int x1 = mx + (int)cur_poly[(i + 1) % n][0], y1 = my + (int)cur_poly[(i + 1) % n][1];
        s_line(&screen, x0, y0, x1, y1, 0x000000);
    }
}

static int launch_match(int idx)
{
    struct app *a = app_at(idx);
    if (!a) return 0;
    if (!launch.q[0]) return 1;
    return strstr(a->id, launch.q) || strstr(a->title, launch.q);
}

static void paint_launcher(void)
{
    const struct theme *t = theme_current();
    int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
    s_fill(&screen, px, py, pw, ph, t->win_bg);
    s_frame_rect(&screen, px, py, pw, ph, t->main);
    s_text(&screen, px + 10, py + 8, "Launch an app (type to search)", t->main);
    s_frame_rect(&screen, px + 10, py + 26, pw - 20, 20, t->main);
    s_text(&screen, px + 14, py + 30, launch.q, t->text);
    if ((tick_count / 50) % 2 == 0)
        s_fill(&screen, px + 14 + s_text_width(launch.q), py + 30, 2, 12, t->main);
    int y = py + 56;
    launch.hover = -1;
    int row = 0;
    for (int i = 0; i < app_count(); i++) {
        if (!launch_match(i)) continue;
        if (row >= 10) break;
        if (in_rect(mx, my, px + 6, y - 3, pw - 12, 22)) {
            launch.hover = i;
            s_fill(&screen, px + 6, y - 3, pw - 12, 22, blend(t->win_bg, t->main, 25));
        }
        s_icon(&screen, app_at(i)->icon, px + 12, y, t->main);
        s_text(&screen, px + 44, y + 4, app_at(i)->title, t->text);
        y += 24; row++;
    }
    if (!row) s_text(&screen, px + 14, y, "(no matching app)", ((t->main >> 1) & 0x7F7F7F));
}

#define CUR_W 14
#define CUR_H 19
static u32 *cur_buf;
static int cur_bx, cur_by, cur_bw, cur_bh, cur_have;
static void cur_draw(void);
static void cur_restore(void);

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
        if (lowest->dirty && lowest->app && lowest->app->paint) {
            lowest->app->paint(lowest);
            lowest->dirty = 0;
        }
        paint_window(lowest);
    }
    paint_menu();
    paint_taskbar();
    if (launch.active) paint_launcher();
    cur_have = 0;                 /* scene was fully redrawn under cursor */
    cur_draw();
    fb_flip();
}

/* ------------------------------------------------------- cursor overlay ----
 * Moving the mouse over plain desktop used to trigger a full-screen
 * composite per event. The cursor now lives in a tiny saved-background
 * overlay: erase old position, save+draw new one, flip. ~300 pixels of
 * memcpy instead of a megapixel repaint.
 */

static void cur_restore(void)
{
    if (!cur_have || !cur_buf) return;
    for (int r = 0; r < cur_bh; r++)
        memcpy(screen.px + (u32)(cur_by + r) * screen_w + cur_bx,
               cur_buf + (u32)r * CUR_W, (u32)cur_bw * 4);
    cur_have = 0;
}

static void cur_draw(void)
{
    cur_restore();
    int x = mx, y = my, w = CUR_W, h = CUR_H;
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    if (!cur_buf) cur_buf = palloc(CUR_W * CUR_H * 4);
    if (!cur_buf) { paint_cursor(); return; }
    for (int r = 0; r < h; r++)
        memcpy(cur_buf + (u32)r * CUR_W,
               screen.px + (u32)(y + r) * screen_w + x, (u32)w * 4);
    cur_bx = x; cur_by = y; cur_bw = w; cur_bh = h;
    paint_cursor();
    cur_have = 1;
}

/* pure cursor move: erase + redraw overlay only */
static void cur_move(void)
{
    cur_restore();
    cur_draw();
    fb_flip();
}

/* does anything hoverable live under the cursor? */
static int move_needs_composite(struct window **wout)
{
    if (menu.active) return 1;
    if (launch.active &&
        in_rect(mx, my, 8, screen_h - TASKBAR_H - 308, 300, 308)) return 1;
    if (my >= screen_h - TASKBAR_H) return 1;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int x, y, ww, hh;
        icon_rect(i, &x, &y, &ww, &hh);
        if (in_rect(mx, my, x, y, ww, hh)) return 1;
    }
    struct window *w = win_at_point(mx, my);
    if (w) { *wout = w; return 1; }
    return 0;
}

/* -------------------------------------------------------------- input ---- */
static void handle_mouse(struct mouse_event *e)
{
    if (e->type == MEV_MOVE) {
        int sens = prefs_get()->mouse_sens;
        mx += e->dx * sens / 3; my -= e->dy * sens / 3;  /* PS/2: +dy is up */
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
        if (band_active) {
            band_x1 = mx; band_y1 = my;
            dirty = 1;
        }
        if (desk_drag >= 0 && !desk_drag_moved &&
            (mx - desk_sx > 6 || mx - desk_sx < -6 || my - desk_sy > 6 || my - desk_sy < -6))
            desk_drag_moved = 1;
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
                    w->dirty = 0;
                }
            }
        }
        if (drag_win || band_active || resize_win || desk_drag >= 0) {
            dirty = 1;
            return;
        }
        struct window *hw = NULL;
        if (!move_needs_composite(&hw)) {
            cur_move();              /* cheap overlay-only cursor move */
            return;
        }
        if (hw) hw->dirty = 1;       /* hover effects inside that window */
        dirty = 1;
        return;
    }
    if (e->type == MEV_WHEEL && my >= screen_h - TASKBAR_H) {
        int x0, x1, vis, scrollable;
        tb_geom(&x0, &x1, &vis, &scrollable);
        if (scrollable) {
            tb_off += (e->wheel > 0) ? 1 : -1;
            if (tb_off < 0) tb_off = 0;
            if (tb_off > win_count - vis) tb_off = win_count - vis;
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
    if (e->down) {
        static u64 last_down_tick;
        static int last_down_x, last_down_y, last_down_btn;
        int dbl = (e->button == last_down_btn &&
                   (tick_count - last_down_tick) <= (u64)prefs_get()->dbl_ms / 10 &&
                   mx - last_down_x <= 6 && mx - last_down_x >= -6 &&
                   my - last_down_y <= 6 && my - last_down_y >= -6);
        last_down_tick = tick_count;
        last_down_x = mx; last_down_y = my; last_down_btn = e->button;
        if (dbl && e->button == MBTN_LEFT && my < screen_h - TASKBAR_H &&
            !win_at_point(mx, my)) {
            for (int i = 0; i < nitems; i++) {
                if (items[i].hidden) continue;
                int x, y, ww, hh;
                icon_rect(i, &x, &y, &ww, &hh);
                if (in_rect(mx, my, x, y, ww, hh)) {
                    desktop_open(&items[i]);
                    dirty = 1;
                    return;
                }
            }
        }
    }
    if (!e->down) {
        if (desk_drag >= 0) {
            if (desk_drag_moved) {
                int gx = (mx - desk_drag_ox + 40 - 16) / 88;
                int gy = (my - desk_drag_oy + 44 - 16) / 96;
                if (gx < 0) gx = 0; if (gx > DESK_COLS - 1) gx = DESK_COLS - 1;
                if (gy < 0) gy = 0; if (gy > DESK_ROWS - 1) gy = DESK_ROWS - 1;
                if (cell_taken(gx, gy, desk_drag)) {       /* nearest free cell */
                    for (int r = 1; r < 8 && cell_taken(gx, gy, desk_drag); r++)
                        for (int dy = -r; dy <= r && cell_taken(gx, gy, desk_drag); dy++)
                            for (int dx = -r; dx <= r && cell_taken(gx, gy, desk_drag); dx++) {
                                int nx = gx + dx, ny = gy + dy;
                                if (nx >= 0 && nx < DESK_COLS && ny >= 0 && ny < DESK_ROWS &&
                                    !cell_taken(nx, ny, desk_drag)) { gx = nx; gy = ny; }
                            }
                }
                items[desk_drag].gx = gx; items[desk_drag].gy = gy;
                desktop_save();
            }
            desk_drag = -1;
        }
        if (band_active) {
            band_active = 0;
            if ((band_x1 - band_x0 > 4 || band_x1 - band_x0 < -4) &&
                (band_y1 - band_y0 > 4 || band_y1 - band_y0 < -4)) {
                int x = band_x0 < band_x1 ? band_x0 : band_x1;
                int y = band_y0 < band_y1 ? band_y0 : band_y1;
                int w = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
                int h = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
                desk_sel = 0;
                for (int i = 0; i < nitems; i++) {
                    if (items[i].hidden) continue;
                    int ix, iy, iw, ih;
                    icon_rect(i, &ix, &iy, &iw, &ih);
                    if (ix < x + w && ix + iw > x && iy < y + h && iy + ih > y)
                        desk_sel |= (1u << i);
                }
            }
        }
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

    if (launch.active) {
        int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
        if (in_rect(mx, my, px, py, pw, ph)) {
            if (launch.hover >= 0) {
                wm_open_app(app_at(launch.hover)->id, NULL);
                launch.active = 0;
            }
            dirty = 1;
            return;
        }
        if (!in_rect(mx, my, 8, screen_h - TASKBAR_H + 6, 34, 28)) {
            launch.active = 0;
            dirty = 1;
            return;
        }
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
                            w->dirty = 1;
                        } else {
                            w->px = w->x; w->py = w->y; w->pw = w->w; w->ph = w->h;
                            win_free_buf(w);
                            w->x = 0; w->y = 0;
                            w->w = screen_w; w->h = screen_h - TASKBAR_H;
                            w->state = WIN_STATE_MAX;
                            w->dirty = 1;
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

    /* desktop items: left = select/drag/open, right = context menu
     * (taskbar area is handled further below) */
    if (e->button == MBTN_LEFT && my < screen_h - TASKBAR_H) {
        for (int i = 0; i < nitems; i++) {
            if (items[i].hidden) continue;
            int x, y, ww, hh;
            icon_rect(i, &x, &y, &ww, &hh);
            if (in_rect(mx, my, x, y, ww, hh)) {
                if (!(desk_sel & (1u << i))) desk_sel = (1u << i);
                desk_drag = i; desk_drag_moved = 0;
                desk_drag_ox = mx - x; desk_drag_oy = my - y;
                desk_sx = mx; desk_sy = my;
                dirty = 1;
                return;
            }
        }
        band_active = 1;
        band_x0 = band_x1 = mx; band_y0 = band_y1 = my;
        desk_sel = 0;
        dirty = 1;
        return;
    }
    if (e->button == MBTN_RIGHT && my < screen_h - TASKBAR_H) {
        int hit = -1;
        for (int i = 0; i < nitems; i++) {
            if (items[i].hidden) continue;
            int x, y, ww, hh;
            icon_rect(i, &x, &y, &ww, &hh);
            if (in_rect(mx, my, x, y, ww, hh)) { hit = i; break; }
        }
        if (hit >= 0) {
            if (!(desk_sel & (1u << hit))) desk_sel = (1u << hit);
            static const char *m[2] = { "Open", "Remove from Desktop" };
            wm_menu(mx, my, m, 2, desk_item_menu_cb, NULL);
        } else if (desk_sel) {
            static const char *m[2] = { "Remove from Desktop", "Clear Selection" };
            wm_menu(mx, my, m, 2, desk_sel_menu_cb, NULL);
        } else {
            static const char *m[1] = { "Restore removed icons" };
            wm_menu(mx, my, m, 1, desk_empty_menu_cb, NULL);
        }
        dirty = 1;
        return;
    }
    /* taskbar */
    if (my >= screen_h - TASKBAR_H) {
        if (in_rect(mx, my, 8, screen_h - TASKBAR_H + 6, 34, TASKBAR_H - 12)) {
            launch.active = !launch.active;
            launch.q[0] = 0; launch.qpos = 0;
            dirty = 1;
            return;
        }
        int px, py, pw, ph;
        tb_power_rect(&px, &py, &pw, &ph);
        if (in_rect(mx, my, px, py, pw, ph)) {
            static const char *items[2] = { "Restart", "Power Off" };
            wm_menu(px - 140, py - 2 * 24 - 8, items, 2, power_menu_cb, NULL);
            dirty = 1;
            return;
        }
        int x0, x1, vis, scrollable;
        tb_geom(&x0, &x1, &vis, &scrollable);
        for (int i = tb_off; i < win_count; i++) {
            int bx = x0 + (i - tb_off) * (TB_BTN_W + TB_BTN_GAP);
            if (bx + TB_BTN_W > x1) break;
            if (in_rect(mx, my, bx, screen_h - TASKBAR_H + 6, TB_BTN_W, TASKBAR_H - 12)) {
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
    if (launch.active) {
        if (e->pressed && e->keycode == 27) { launch.active = 0; dirty = 1; return; }
        if (e->pressed && e->keycode == '\n') {
            for (int i = 0; i < app_count(); i++)
                if (launch_match(i)) { wm_open_app(app_at(i)->id, NULL); break; }
            launch.active = 0;
            dirty = 1;
            return;
        }
        edit_line(launch.q, &launch.qpos, sizeof(launch.q), e);
        dirty = 1;
        return;
    }
    if (e->pressed && e->keycode == KEY_DELETE && desk_sel && !focused_w && !modal_w) {
        desk_remove_sel();
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
        for (int i = 0; i < win_count; i++)
            if (wins[i].state != WIN_STATE_MIN) wins[i].dirty = 1;
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

static u32 wm_t0;
static int diag_tried;

static int is_v86_box(void)
{
    const char *m = ata_model();
    return m && strstr(m, "v86");
}

void wm_run(void)
{
    wm_t0 = tick_count;
    input_guard_armed = 1;
    if (usb_diag_flag()) diag_run();   /* enumeration already reported trouble */

    klog("wm: entering main loop");
    for (;;) {
        irq_watchdog();
        usb_poll();
        if (!diag_tried && !input_last_tick && !is_v86_box() &&
            tick_count - wm_t0 > 600) {   /* 6 s of total silence on real HW */
            diag_tried = 1;
            diag_run();
        }
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
        cpu_idle_begin();          /* TSC-mark the halt so usage = 100-idle */
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
