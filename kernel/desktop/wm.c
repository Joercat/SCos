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
static void menu_cancel(void);
#define BTN_SZ 18

/* Stable slots: app/tab/dialog back-pointers must survive closing an earlier window. */
static struct window window_slots[MAX_WINDOWS];
static struct window *wins[MAX_WINDOWS];
static int win_count;
static int next_z = 10, next_id = 1;
static struct window *focused_w;
static struct window *modal_w;
static int dirty = 1;

/* r39: per-app CPU accounting. Every app callback (paint/tick/mouse/
 * key) is timed with the TSC; once per second the accumulated cycles
 * become a percent of the machine's measured TSC rate (the same
 * calibration SysMon already shows as MHz). Real numbers only: on a
 * box without TSC nothing is instrumented and the column reads 0. */
static u64 app_cyc[MAX_WINDOWS];
static u32 app_pct[MAX_WINDOWS];
static int cyc_ok;                     /* TSC usable (set in wm_run) */
#define APP_T0(w) u64 cyc_t0 = cyc_ok ? cpu_busy_cycles() : 0; int cyc_id = (w)->id; uintptr_t prior_owner=heap_set_owner((uintptr_t)(w))
#define APP_T1(w) do { heap_set_owner(prior_owner); if (cyc_ok) { \
    u64 elapsed = cpu_busy_cycles() - cyc_t0; \
    for (int ci = 0; ci < win_count; ci++) \
        if (wins[ci]->id == cyc_id) { app_cyc[ci] += elapsed; break; } \
    } } while (0)

static void cyc_reset_all(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) { app_cyc[i] = 0; app_pct[i] = 0; }
}

u32 wm_win_cpu_pct(struct window *w)
{
    if (!w) return 0;
    for(int i=0;i<win_count;i++)if(wins[i]==w)return app_pct[i];
    return 0;
}

/* ---- damage-rect compositing: repaint and flip only what changed ---- */
struct drect { int x, y, w, h; };
static struct drect dmg[8];
static int ndmg;
static int full_dirty;
static int tb_dirty, icons_dirty, menu_dirty, launch_dirty;

static void wm_full(void) { dirty = 1; full_dirty = 1; }

/* held full-screen overlays (diagnostics, error screen) paint straight to
 * the framebuffer; when they hand control back, the WM's dirty-region
 * bookkeeping is stale and partial repaints (the taskbar clock) would draw
 * OVER the leftover overlay until some unrelated event dirtied everything.
 * Overlay code calls this on exit so the next paint is a full one. */
void wm_request_full(void) { wm_full(); }

static int mx = 300, my = 300;
static int mouse_rx, mouse_ry, mouse_sens;
static u8 mbuttons;
static struct window *drag_win, *resize_win;
static int drag_ox, drag_oy, resize_ox, resize_oy;

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
    if(item<0)return;
    (void)ud;
    sleep_ms(400);
    usb_kbd_leds_off();                    /* no lit LEDs on standby power */
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
struct window *wm_win_at(int i) { return i>=0&&i<win_count?wins[i]:NULL; }
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
    w->surf.px = palloc_owned((size_t)cw * ch * 4,(uintptr_t)w);
    w->surf.w = cw;
    w->surf.h = ch;
    if (w->surf.px) memset(w->surf.px, 0, (u32)cw * ch * 4);
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
    w->chrome_dirty = 1;
    dirty = 1;
}

void wm_focus(struct window *w)
{
    if (!w) return;
    w->z = ++next_z;
    focused_w = w;
    wm_full();
}

static struct window *win_at_point(int x, int y)
{
    struct window *best = NULL;
    for (int i = 0; i < win_count; i++) {
        struct window *w = wins[i];
        if (w->state == WIN_STATE_MIN) continue;
        if (in_rect(x, y, w->x, w->y, w->w, w->h))
            if (!best || w->z > best->z) best = w;
    }
    return best;
}

int wm_app_running(const char *app_id)
{
    struct app *a = app_find(app_id);
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < win_count; i++)
        if (wins[i]->app == a) n++;
    return n;
}

void wm_track_mem(struct window *w, ptrdiff_t delta)
{
    if (!w) return;
    if(delta<0){size_t n=0-(size_t)delta;w->data_bytes=n>w->data_bytes?0:w->data_bytes-n;}
    else if((size_t)delta<=SIZE_MAX-w->data_bytes)w->data_bytes+=(size_t)delta;
    else panic("window memory accounting overflow");
}

void wm_set_console(struct window *w, void *term)
{
    if (w) w->console = term;
}

static void *pending_console;      /* consumed by the next wm_open_app */
void wm_set_pending_console(void *term)
{
    pending_console = term;
}

void wm_clear_console(void *term)
{
    for (int i = 0; i < win_count; i++)
        if (wins[i]->console == term) wins[i]->console = NULL;
}

/* one real app event -> the terminal tab that launched this app, like a
 * Linux program writing to the console it was started from */
void app_log(struct window *w, const char *line)
{
    if (!w || !w->console || !line || !line[0]) return;
    char buf[208];
    strcpy(buf, "[");
    strncat(buf, w->app ? w->app->id : "app", 24);
    strcat(buf, "] ");
    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    term_console_line(w->console, buf);
}

void wm_close_window(struct window *w)
{
    menu_cancel();
    if (!w || w->closing) return;
    w->closing = 1;
    if (w->console && w->app)
        term_console_exit(w->console, w->app->id);
    if (w->app && w->app->close) w->app->close(w);
    win_free_buf(w);
    int idx = -1;
    for (int i = 0; i < win_count; i++) if (wins[i] == w) idx = i;
    if (idx >= 0) {
        for (int i = idx; i < win_count - 1; i++) wins[i] = wins[i + 1];
        win_count--;
        cyc_reset_all();   /* r39: window indices shifted; CPU% rebuilds
                            * from zero within the next second */
    }
    w->app=NULL;w->data=NULL;
    if (focused_w == w) focused_w = NULL;
    if (modal_w == w) modal_w = NULL;
    if (drag_win == w) drag_win = NULL;
    if (resize_win == w) resize_win = NULL;
    if (!focused_w) {
        struct window *top = NULL;
        for (int i = 0; i < win_count; i++)
            if (!top || wins[i]->z > top->z) top = wins[i];
        focused_w = top;
    }
    wm_full();
}

struct window *wm_open_app(const char *app_id, void *arg)
{
    /* console hand-off: `appstrt` parks the launching terminal tab here so
     * the app's open()-time log lines already reach it */
    void *cons = pending_console;
    pending_console = NULL;
    struct app *app = app_find(app_id);
    if (!app) return NULL;
    if (app->single)
        for (int i = 0; i < win_count; i++)
            if (wins[i]->app == app) {
                if (wins[i]->state == WIN_STATE_MIN) wins[i]->state = WIN_STATE_NORMAL;
                if (cons) wins[i]->console = cons;
                wm_focus(wins[i]);
                return wins[i];
            }
    if (win_count >= MAX_WINDOWS) return NULL;
    struct window *w = NULL;
    for(int i=0;i<MAX_WINDOWS;i++)if(!window_slots[i].app){w=&window_slots[i];break;}
    if(!w)return NULL;
    wins[win_count++]=w;
    cyc_reset_all();
    memset(w, 0, sizeof(*w));
    w->console = cons;
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
    if (!w->surf.px) {
        win_count--;
        w->app=NULL;
        err_notify(app->id,
                   "Not enough memory to open this window. Close some "
                   "windows or apps and try again.", NULL, 0);
        return NULL;
    }
    w->dirty = 1;
    if (app->open) { APP_T0(w);app->open(w, arg);APP_T1(w); }
    if (app->uses_data && !w->data) {
        win_count--;             /* app could not allocate its state */
        win_free_buf(w);w->app=NULL;
        err_notify(app->id,
                   "Not enough memory to open this app. Close some "
                   "windows or apps and try again.", NULL, 0);
        return NULL;
    }
    focused_w = w;
    wm_full();
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
    wm_full();
}

/* -------------------------------------------------------------- menus ---- */
static void menu_cancel(void){
    if(!menu.active)return;
    menu.active=0;
    if(menu.cb)menu.cb(-1,menu.ud);
}
void wm_menu(int x, int y, const char **items, int n, menu_cb cb, void *ud)
{
    menu_cancel();
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
    menu_dirty = 1;
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
            wp_cache.px = palloc_owned((size_t)screen_w * screen_h * 4,HEAP_WM);
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
            /* r27: the decorative "corner glow" discs are gone - the user
             * saw them as a weird circle floating in the top-right of the
             * desktop. The wallpaper is now just gradient + grid. */
            wp_valid = 1;
        }
    }
    if (wp_valid)
        memcpy(screen.px, wp_cache.px, (u32)screen_w * screen_h * 4);
    else
        s_fill(&screen, 0, 0, screen_w, screen_h, t->bg_top);
}

static int icon_band = 232;   /* px height of the icon region actually in use */
static int in_full;           /* paint_all is running: skip partial restores */

/* restore one rectangle of clean wallpaper into the back buffer (used by
 * partial repaints to erase stale hover/selection/cursor pixels) */
static void wp_restore_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    if (wp_valid) {
        for (int r = 0; r < h; r++)
            memcpy(screen.px + (u32)(y + r) * screen_w + x,
                   wp_cache.px + (u32)(y + r) * screen_w + x, (u32)w * 4);
    } else {
        s_fill(&screen, x, y, w, h, theme_current()->bg_top);
    }
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
static struct { int active; char q[48]; int qpos; int hover, offset; } launch;

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
        for (int i = 0; i < app_count() && nitems < DESK_MAX; i++) {
            const struct app *a = app_at(i);
            if (a->id[0] == '_') continue;
            struct ditem *d = &items[nitems++];
            d->kind = 0;
            strncpy(d->app, a->id, 31);
            d->path[0] = 0;
            strncpy(d->label, a->desktop_label ? a->desktop_label : a->title, 39);
            d->gx = (nitems - 1) % 8; d->gy = (nitems - 1) / 8; d->hidden = 0;
        }
    }
}
static void desktop_open(struct ditem *d)
{
    if (d->kind == 0) wm_open_app(d->app, NULL);
    else app_open_document(d->path);
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
    icons_dirty = 1;
    dirty = 1;
}
static void desk_item_menu_cb(int item, void *ud)
{
    if(item<0)return;
    (void)ud;
    if (item == 0) {
        for (int i = 0; i < nitems; i++)
            if (desk_sel & (1u << i)) { desktop_open(&items[i]); break; }
    } else desk_remove_sel();
}
/* bring back every hidden app icon and re-add missing registered apps */
static void desk_restore(void)
{
    lua_apps_refresh();
    for (int i = 0; i < nitems; i++)
        if (items[i].kind == 0) items[i].hidden = 0;
    for (int a = 0; a < app_count(); a++) {
        const struct app *ap = app_at(a);
        if (ap->id[0] == '_') continue;
        int found = 0;
        for (int i = 0; i < nitems; i++)
            if (items[i].kind == 0 && strcmp(items[i].app, ap->id) == 0) found = 1;
        if (found || nitems >= DESK_MAX) continue;
        struct ditem *d = &items[nitems];
        memset(d, 0, sizeof(*d));
        d->kind = 0;
        strcpy(d->app, ap->id);
        strcpy(d->label, ap->desktop_label ? ap->desktop_label : ap->title);
        int placed = 0;
        for (int gy = 0; gy < DESK_ROWS && !placed; gy++)
            for (int gx = 0; gx < DESK_COLS && !placed; gx++)
                if (!cell_taken(gx, gy, -1)) { d->gx = gx; d->gy = gy; placed = 1; }
        if (placed) nitems++;
    }
    desktop_save();
    icons_dirty = 1;
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
    if(item<0)return;
    (void)item; (void)ud;
    desk_restore();
}

static void desk_sel_menu_cb(int item, void *ud)
{
    if(item<0)return;
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
    /* restore the whole icon band from clean wallpaper, not just the cells:
     * a cursor burnt into a GAP between cells used to survive the repaint,
     * get baked into cur_buf as fake "background", and stamp sticky ghost
     * spots everywhere the cursor went afterwards */
    int band = 16;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int bot = 16 + items[i].gy * 96 + 88;
        if (bot > band) band = bot;
    }
    band += 8;
    icon_band = band;
    if (!in_full) wp_restore_rect(0, 0, screen_w, band);
    /* no hover feedback while dragging/resizing/modally busy: the pointer is
     * occupied and highlighting launch buttons underneath is misleading */
    int interactive = !drag_win && !resize_win && desk_drag < 0 &&
                      !band_active && !menu.active && !launch.active && !modal_w &&
                      !win_at_point(mx, my);
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

static void paint_win_chrome(struct window *w)
{
    const struct theme *t = theme_current();
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    s_fill(&screen, x + 4, y + 4, ww, hh, blend(t->bg_bot, 0x000000, 60));
    u32 title_bg = (focused_w == w) ? t->main : blend(t->main, t->win_bg, 55);
    s_fill(&screen, x, y, ww, hh, t->win_bg);
    s_frame_rect(&screen, x, y, ww, hh, t->main);
    s_fill(&screen, x + 1, y + 1, ww - 2, WIN_TITLEBAR - 2, title_bg);
    u32 title_fg = (focused_w == w) ? t->title_text : t->text;
    s_clip_text(&screen, x + 8, y + 5, w->title, title_fg, ww - 90);
    for (int b = 0; b < 3; b++)
        draw_win_button(&screen, x + ww - 22 - (2 - b) * 22, y + 3, b, title_fg);
    u32 hz = blend(t->win_bg, t->main, 60);
    for (int i = 0; i < 4; i++)
        s_fill(&screen, x + ww - 12 + i * 3, y + hh - 12 + i * 3, 2, 2, hz);
}

static void paint_win_content(struct window *w)
{
    if (!w->surf.px) return;     /* allocation failed - chrome only */
    s_blit(&screen, &w->surf, w->x + 1, w->y + WIN_TITLEBAR);
}

static void paint_window(struct window *w)
{
    if (w->state == WIN_STATE_MIN) return;
    paint_win_chrome(w);
    paint_win_content(w);
}

#define CUR_W 14
#define CUR_H 19
static u32 *cur_buf;
static int cur_bx, cur_by, cur_bw, cur_bh, cur_have;
static void cur_draw(void);
static void cur_restore(void);
static void paint_icons(void);
static void paint_taskbar(void);
static void paint_menu(void);
static void paint_launcher(void);


static void damage_add(int x, int y, int w, int h)
{
    if (ndmg >= 8) { ndmg = 0; dmg[0].x = 0; dmg[0].y = 0;
                    dmg[0].w = screen_w; dmg[0].h = screen_h; ndmg = 1; return; }
    for (int i = 0; i < ndmg; i++) {
        struct drect *r = &dmg[i];
        if (x < r->x + r->w + 8 && r->x < x + w + 8 &&
            y < r->y + r->h + 8 && r->y < y + h + 8) {
            int x1 = r->x < x ? r->x : x, y1 = r->y < y ? r->y : y;
            int x2 = r->x + r->w > x + w ? r->x + r->w : x + w;
            int y2 = r->y + r->h > y + h ? r->y + r->h : y + h;
            r->x = x1; r->y = y1; r->w = x2 - x1; r->h = y2 - y1;
            return;
        }
    }
    dmg[ndmg].x = x; dmg[ndmg].y = y; dmg[ndmg].w = w; dmg[ndmg].h = h;
    ndmg++;
}

static int dmg_intersects(int x, int y, int w, int h)
{
    for (int i = 0; i < ndmg; i++)
        if (x < dmg[i].x + dmg[i].w && dmg[i].x < x + w &&
            y < dmg[i].y + dmg[i].h && dmg[i].y < y + h)
            return 1;
    return 0;
}

static void paint_partial(void)
{
    /* r34 ghost-cursor fix: the back buffer must be cursor-free BEFORE
     * anything saves a new cursor background.  The old code cleared
     * cur_have here and let cur_draw() "save" a background that still
     * contained the OLD cursor pixels wherever the damage rects had not
     * repainted - every later restore then stamped dead cursor copies
     * into the scene (the field report's "dead copies of the mouse").
     * Invariant now: back buffer holds cursor pixels iff cur_have == 1,
     * so a restore at the top always leaves a clean scene. */
    if (cur_have) {
        damage_add(cur_bx, cur_by, cur_bw, cur_bh);
        cur_restore();
    }
    if (icons_dirty) {
        paint_icons();
        damage_add(0, 0, screen_w, icon_band);
        icons_dirty = 0;
    }
    /* Pass 1: repaint dirty windows bottom-up in z-order, so a higher dirty
     * window always blits after a lower one it overlaps. */
    static u8 drawn[MAX_WINDOWS];
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    for (int pass = 0; pass < win_count; pass++) {
        struct window *lowest = NULL;
        for (int i = 0; i < win_count; i++) {
            if (drawn[i] || wins[i]->state == WIN_STATE_MIN) continue;
            if (!lowest || wins[i]->z < lowest->z) lowest = wins[i];
        }
        if (!lowest) break;
        for(int di=0;di<win_count;di++)if(wins[di]==lowest){drawn[di]=1;break;}
        if (lowest->dirty && lowest->app && lowest->app->paint) {
            { APP_T0(lowest); lowest->app->paint(lowest); APP_T1(lowest); }
            lowest->dirty = 0;
            paint_win_content(lowest);
            damage_add(lowest->x + 1, lowest->y + WIN_TITLEBAR, lowest->w - 2,
                       lowest->h - WIN_TITLEBAR - 1);
        }
        if (lowest->chrome_dirty) {
            lowest->chrome_dirty = 0;
            /* chrome paints the whole frame background first, so the content
             * surface must be re-blitted over it in the same pass */
            paint_window(lowest);
            damage_add(lowest->x, lowest->y, lowest->w + 4, lowest->h + 4);
        }
    }
    /* Pass 2: occlusion repair. Any window overlapping accumulated damage
     * that was NOT repainted gets re-blitted, so a lower window's repaint
     * (or icon/cursor erase) can never bleed through a window above it. */
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    for (int pass = 0; pass < win_count; pass++) {
        struct window *lowest = NULL;
        for (int i = 0; i < win_count; i++) {
            if (drawn[i] || wins[i]->state == WIN_STATE_MIN) continue;
            if (!lowest || wins[i]->z < lowest->z) lowest = wins[i];
        }
        if (!lowest) break;
        for(int di=0;di<win_count;di++)if(wins[di]==lowest){drawn[di]=1;break;}
        if (dmg_intersects(lowest->x, lowest->y, lowest->w + 4, lowest->h + 4)) {
            paint_window(lowest);
            /* Whole-frame blits can touch higher windows outside the
             * original damage. Propagate those bounds up the z stack. */
            damage_add(lowest->x, lowest->y, lowest->w + 4, lowest->h + 4);
        }
    }
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    if (tb_dirty || dmg_intersects(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H)) {
        paint_taskbar();
        damage_add(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
        tb_dirty = 0;
    }
    if (menu.active && (menu_dirty || dmg_intersects(menu.x, menu.y, menu.w, menu.h))) {
        paint_menu();
        damage_add(menu.x, menu.y, menu.w, menu.h);
        menu_dirty = 0;
    }
    if (launch.active && (launch_dirty ||
        dmg_intersects(8, screen_h - 348, 300, 308))) {
        paint_launcher();
        damage_add(8, screen_h - 348, 300, 308);
        launch_dirty = 0;
    }
    /* the scene was repainted cursor-free (restore at the top), so stamp
     * the pointer now - cur_draw saves a provably clean background - and
     * include its rect in the flip */
    cur_draw();
    damage_add(mx, my, CUR_W, CUR_H);
    for (int i = 0; i < ndmg; i++)
        fb_flip_rect(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
    ndmg = 0;
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
        struct window *w = wins[i];
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
    power_hover = in_rect(mx, my, px, py, pw, ph) && !drag_win && !resize_win &&
                  desk_drag < 0 && !band_active && !menu.active && !modal_w;
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
    if (!a || a->id[0] == '_') return 0;
    if (!launch.q[0]) return 1;
    return strstr(a->id, launch.q) || strstr(a->title, launch.q);
}

/* Hit testing must not depend on the previous rendered hover frame. */
static int launcher_at_point(void)
{
    int row = 0, skipped = 0;
    for (int i = 0; i < app_count() && row < 10; i++) {
        if (!launch_match(i)) continue;
        if (skipped++ < launch.offset) continue;
        if (in_rect(mx, my, 14, screen_h - TASKBAR_H - 255 + row * 24, 288, 22)) return i;
        row++;
    }
    return -1;
}

static void paint_launcher(void)
{
    const struct theme *t = theme_current();
    int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
    s_fill(&screen, px, py, pw, ph, t->win_bg);
    s_frame_rect(&screen, px, py, pw, ph, t->main);
    s_text(&screen, px + 10, py + 8, "Apps: type to search / scroll", t->main);
    s_frame_rect(&screen, px + 10, py + 26, pw - 20, 20, t->main);
    s_text(&screen, px + 14, py + 30, launch.q, t->text);
    if ((tick_count / 50) % 2 == 0)
        s_fill(&screen, px + 14 + s_text_width(launch.q), py + 30, 2, 12, t->main);
    int y = py + 56;
    launch.hover = -1;
    int row = 0, skipped = 0;
    for (int i = 0; i < app_count(); i++) {
        if (!launch_match(i)) continue;
        if (skipped++ < launch.offset) continue;
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


static void paint_all(void)
{
    paint_wallpaper();
    in_full = 1;
    paint_icons();
    in_full = 0;
    /* windows in z order (bottom first) */
    static u8 drawn[MAX_WINDOWS];
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    for (int pass = 0; pass < win_count; pass++) {
        struct window *lowest = NULL;
        for (int i = 0; i < win_count; i++) {
            if (drawn[i] || wins[i]->state == WIN_STATE_MIN) continue;
            if (!lowest || wins[i]->z < lowest->z) lowest = wins[i];
        }
        if (!lowest) break;
        for(int di=0;di<win_count;di++)if(wins[di]==lowest){drawn[di]=1;break;}
        if (lowest->dirty && lowest->app && lowest->app->paint) {
            { APP_T0(lowest); lowest->app->paint(lowest); APP_T1(lowest); }
            lowest->dirty = 0;
        }
        paint_window(lowest);
    }
    paint_menu();
    paint_taskbar();
    if (launch.active) paint_launcher();
    tb_dirty = 0; icons_dirty = 0; menu_dirty = 0; launch_dirty = 0; ndmg = 0;
    for (int i = 0; i < win_count; i++) wins[i]->chrome_dirty = 0;
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
    if (!cur_buf) cur_buf = palloc_owned(CUR_W * CUR_H * 4,HEAP_WM);
    if (!cur_buf) { paint_cursor(); return; }
    for (int r = 0; r < h; r++)
        memcpy(cur_buf + (u32)r * CUR_W,
               screen.px + (u32)(y + r) * screen_w + x, (u32)w * 4);
    cur_bx = x; cur_by = y; cur_bw = w; cur_bh = h;
    paint_cursor();
    cur_have = 1;
}

/* pure cursor move: erase + redraw overlay only, flip the damaged union */
static void cur_move(void)
{
    int ox = cur_bx, oy = cur_by, ow = cur_bw, oh = cur_bh, have = cur_have;
    cur_restore();
    cur_draw();
    int x0 = mx, y0 = my, x1 = mx + CUR_W, y1 = my + CUR_H;
    if (have) {
        if (ox < x0) x0 = ox;
        if (oy < y0) y0 = oy;
        if (ox + ow > x1) x1 = ox + ow;
        if (oy + oh > y1) y1 = oy + oh;
    }
    fb_flip_rect(x0, y0, x1 - x0, y1 - y0);
}

/* does anything hoverable live under the cursor? */
static int move_needs_composite(struct window **wout)
{
    if (menu.active) return 1;
    if (launch.active &&
        in_rect(mx, my, 8, screen_h - TASKBAR_H - 308, 300, 308)) return 1;
    if (my >= screen_h - TASKBAR_H) return 1;
    struct window *w = win_at_point(mx, my);
    if (w) { *wout = w; return 1; }
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int x, y, ww, hh;
        icon_rect(i, &x, &y, &ww, &hh);
        if (in_rect(mx, my, x, y, ww, hh)) return 1;
    }
    return 0;
}

/* one frame of a window drag / resize / rubber-band / icon drag: restore the
 * union region to clean wallpaper, mark everything overlapping it for the
 * damage compositor, and let paint_partial do a targeted repaint. Replaces
 * the old full-screen wm_full() per mouse-move (which burned ~80% CPU). */
static void interact_repaint(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    /* Remove the saved cursor BEFORE changing its underlying scene.
     * Restoring it later would paste old window pixels onto wallpaper. */
    if (cur_have) {
        damage_add(cur_bx, cur_by, cur_bw, cur_bh);
        cur_restore();
    }
    wp_restore_rect(x, y, w, h);
    if (y < icon_band + 8) icons_dirty = 1;
    for (int i = 0; i < win_count; i++) {
        struct window *v = wins[i];
        if (v->state == WIN_STATE_MIN) continue;
        if (x < v->x + v->w + 4 && v->x < x + w + 4 &&
            y < v->y + v->h + 4 && v->y < y + h + 4)
            v->chrome_dirty = 1;
    }
    damage_add(x, y, w, h);
    dirty = 1;
}

/* r36 RESIZE PERF ROOT FIX: every mouse report during a resize used to
 * run palloc + pfree + a full-surface memset + a full app repaint.  At
 * a USB mouse's 125-1000 Hz report rate that is megabytes of memset and
 * repaint per SECOND of dragging - the field report's "resizing windows
 * immediately makes CPU go to 100% and lags the system really bad".
 * Now handle_mouse only records the target geometry; resize_flush()
 * applies it at most once per ~20 ms (50 Hz) from the main loop, and
 * unconditionally (force) when the button releases. */
static int rs_nw, rs_nh, rs_pending;
static u32 rs_last;
static void resize_flush(int force)
{
    if (!resize_win || !rs_pending) return;
    if (!force && rs_last && (u32)tick_count - rs_last < 2) return;
    struct window *w = resize_win;
    int nw = rs_nw, nh = rs_nh;
    rs_pending = 0;
    rs_last = (u32)tick_count;
    if (nw == w->w && nh == w->h) return;
    int cw = nw - 2, ch = nh - WIN_TITLEBAR - 1;
    u32 *np = palloc_owned((size_t)cw * ch * 4,(uintptr_t)w);
    if (!np) return;                       /* OOM: keep the old geometry */
    pfree(w->surf.px, (u32)w->surf.w * w->surf.h * 4);
    w->w = nw; w->h = nh;
    w->surf.px = np; w->surf.w = cw; w->surf.h = ch;
    memset(np, 0, (u32)cw * ch * 4);
    if (w->app && w->app->paint) { APP_T0(w); w->app->paint(w); APP_T1(w); }
    w->dirty = 0;
    damage_add(w->x, w->y, w->w + 4, w->h + 4);
}

/* -------------------------------------------------------------- input ---- */
static void handle_mouse(struct mouse_event *e)
{
    if (e->type == MEV_MOVE) {
        /* pre-move geometry for the interaction damage union */
        int omx = mx, omy = my;
        int odrag = 0, odx = 0, ody = 0, odw = 0, odh = 0;
        if (drag_win) {
            odrag = 1;
            odx = drag_win->x; ody = drag_win->y;
            odw = drag_win->w; odh = drag_win->h;
        }
        int ores = 0, orx = 0, ory = 0, orw = 0, orh = 0;
        if (resize_win) {
            ores = 1;
            orx = resize_win->x; ory = resize_win->y;
            orw = resize_win->w; orh = resize_win->h;
        }
        int obx1 = band_x1, oby1 = band_y1;
        int sens = prefs_get()->mouse_sens;
        if (sens != mouse_sens) {
            mouse_rx = mouse_ry = 0;
            mouse_sens = sens;
        }
        /* Carry subpixel movement across reports, including negative
         * deltas. Low sensitivity must not discard slow mouse motion. */
        int dx = e->dx * sens + mouse_rx;
        int dy = -e->dy * sens + mouse_ry; /* PS/2: +dy is up */
        mx += dx / 3; my += dy / 3;
        mouse_rx = dx % 3; mouse_ry = dy % 3;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > screen_w - 1) mx = screen_w - 1;
        if (my > screen_h - 1) my = screen_h - 1;
        if ((mx == 0 && mouse_rx < 0) || (mx == screen_w - 1 && mouse_rx > 0)) mouse_rx = 0;
        if ((my == 0 && mouse_ry < 0) || (my == screen_h - 1 && mouse_ry > 0)) mouse_ry = 0;
        if (drag_win) {
            drag_win->x = mx - drag_ox;
            drag_win->y = my - drag_oy;
            if (drag_win->y < 0) drag_win->y = 0;
            if (drag_win->y > screen_h - TASKBAR_H - WIN_TITLEBAR)
                drag_win->y = screen_h - TASKBAR_H - WIN_TITLEBAR;
        }
        if (band_active) {
            band_x1 = mx; band_y1 = my;
        }
        if (desk_drag >= 0 && !desk_drag_moved &&
            (mx - desk_sx > 6 || mx - desk_sx < -6 || my - desk_sy > 6 || my - desk_sy < -6))
            desk_drag_moved = 1;
        if (resize_win) {
            struct window *w = resize_win;
            int nw = mx - w->x + resize_ox, nh = my - w->y + resize_oy;
            int minw = 320, minh = 200;
            if (w->app) {
                if (w->app->min_w > minw) minw = w->app->min_w;
                if (w->app->min_h > minh) minh = w->app->min_h;
            }
            if (nw < minw) nw = minw;
            if (nh < minh) nh = minh;
            if (nw > screen_w) nw = screen_w;
            if (nh > screen_h - TASKBAR_H) nh = screen_h - TASKBAR_H;
            if (nw != w->w || nh != w->h) {
                /* r36: record the target only - resize_flush() applies
                 * it once per frame, throttled to ~50 Hz (see above) */
                rs_nw = nw; rs_nh = nh; rs_pending = 1;
            }
        }
        if (drag_win || band_active || resize_win || desk_drag >= 0) {
            int x0 = omx < mx ? omx : mx, y0 = omy < my ? omy : my;
            int x1 = (omx > mx ? omx : mx) + CUR_W;
            int y1 = (omy > my ? omy : my) + CUR_H;
#define UN(ux, uy, uw, uh) do { \
                int a0_ = (ux), b0_ = (uy), a1_ = (ux) + (uw), b1_ = (uy) + (uh); \
                if (a0_ < x0) { x0 = a0_; } \
                if (b0_ < y0) { y0 = b0_; } \
                if (a1_ > x1) { x1 = a1_; } \
                if (b1_ > y1) { y1 = b1_; } \
            } while (0)
            if (odrag) UN(odx, ody, odw + 4, odh + 4);
            if (drag_win) UN(drag_win->x, drag_win->y, drag_win->w + 4, drag_win->h + 4);
            if (ores) UN(orx, ory, orw + 4, orh + 4);
            if (resize_win) UN(resize_win->x, resize_win->y,
                               resize_win->w + 4, resize_win->h + 4);
            if (band_active) {
                int bx = band_x0 < band_x1 ? band_x0 : band_x1;
                int by = band_y0 < band_y1 ? band_y0 : band_y1;
                int bw = (band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1) + 2;
                int bh = (band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1) + 2;
                UN(bx, by, bw, bh);
                int ox = band_x0 < obx1 ? band_x0 : obx1;
                int oy = band_y0 < oby1 ? band_y0 : oby1;
                int ow = (band_x0 < obx1 ? obx1 - band_x0 : band_x0 - obx1) + 2;
                int oh = (band_y0 < oby1 ? oby1 - band_y0 : band_y0 - oby1) + 2;
                UN(ox, oy, ow, oh);
            }
            if (desk_drag >= 0) {
                int ix, iy, iw, ih;
                icon_rect(desk_drag, &ix, &iy, &iw, &ih);
                UN(ix, iy, iw, ih);
                if (desk_drag_moved) {
                    UN(omx - desk_drag_ox, omy - desk_drag_oy, iw + 2, ih + 2);
                    UN(mx - desk_drag_ox, my - desk_drag_oy, iw + 2, ih + 2);
                }
            }
#undef UN
            interact_repaint(x0, y0, x1 - x0, y1 - y0);
            return;
        }
        struct window *hw = NULL;
        int zone = move_needs_composite(&hw);
        /* hover signature: composite only when the hover TARGET changes,
         * not on every pixel of motion; apps get the move event and repaint
         * their own surface only when their internal hover state flips */
        static struct window *last_hw;
        static int last_zone, last_icon = -2, last_tb = -2;
        static int last_mrow = -2, last_lrow = -2;
        int icon = -1, tbz = -1, mrow = -1, lrow = -1;
        if (menu.active) {
            for (int i = 0; i < menu.n; i++)
                if (in_rect(mx, my, menu.x, menu.y + 3 + i * 24, menu.w, 24)) mrow = i;
        } else if (launch.active &&
                   in_rect(mx, my, 8, screen_h - TASKBAR_H - 308, 300, 308)) {
            lrow = launcher_at_point();
        } else if (my >= screen_h - TASKBAR_H) {
            int x, y, w, h;
            tb_power_rect(&x, &y, &w, &h);
            tbz = in_rect(mx, my, x, y, w, h);
        } else if (!hw) {
            for (int i = 0; i < nitems; i++) {
                if (items[i].hidden) continue;
                int x, y, ww, hh;
                icon_rect(i, &x, &y, &ww, &hh);
                if (in_rect(mx, my, x, y, ww, hh)) { icon = i; break; }
            }
        }
        if (hw != last_hw || zone != last_zone || icon != last_icon ||
            tbz != last_tb || mrow != last_mrow || lrow != last_lrow) {
            if (icon != last_icon) icons_dirty = 1;
            if (tbz != last_tb) tb_dirty = 1;
            if (mrow != last_mrow) menu_dirty = 1;
            if (lrow != last_lrow) launch_dirty = 1;
            last_hw = hw; last_zone = zone; last_icon = icon; last_tb = tbz;
            last_mrow = mrow; last_lrow = lrow;
            dirty = 1;
        }
        if (hw && hw->app && hw->app->mouse)
            { APP_T0(hw); hw->app->mouse(hw, e, mx - (hw->x + 1), my - (hw->y + WIN_TITLEBAR)); APP_T1(hw); }
        cur_move();                  /* cheap overlay-only cursor move */
        (void)zone;
        return;
    }
    if (e->type == MEV_WHEEL && launch.active &&
        in_rect(mx,my,8,screen_h-TASKBAR_H-308,300,308)) {
        int count=0;
        for(int i=0;i<app_count();i++)if(launch_match(i))count++;
        launch.offset += e->wheel>0 ? -1 : 1;
        if(launch.offset>count-10)launch.offset=count>10?count-10:0;
        if(launch.offset<0)launch.offset=0;
        launch_dirty=1;dirty=1;return;
    }
    if (e->type == MEV_WHEEL && my >= screen_h - TASKBAR_H) {
        int x0, x1, vis, scrollable;
        tb_geom(&x0, &x1, &vis, &scrollable);
        if (scrollable) {
            tb_off += (e->wheel > 0) ? -1 : 1;
            if (tb_off < 0) tb_off = 0;
            if (tb_off > win_count - vis) tb_off = win_count - vis;
        }
        tb_dirty = 1;
        dirty = 1;
        return;
    }
    if (e->type == MEV_WHEEL) {
        struct window *w = win_at_point(mx, my);
        if (w && w->app && w->app->mouse) {
            { APP_T0(w); w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR)); APP_T1(w); }
            w->dirty = 1;            /* app scrolled: repaint its content */
        }
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
                if (gx < 0) gx = 0;
                if (gx > DESK_COLS - 1) gx = DESK_COLS - 1;
                if (gy < 0) gy = 0;
                if (gy > DESK_ROWS - 1) gy = DESK_ROWS - 1;
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
        resize_flush(1);          /* r36: apply the final resize target */
        resize_win = NULL;
        wm_full();
        return;
    }

    if (menu.active) {
        if (in_rect(mx, my, menu.x, menu.y, menu.w, menu.h)) {
            int item = my < menu.y + 3 ? -1 : (my - menu.y - 3) / 24;
            menu_cb cb = menu.cb;
            void *ud = menu.ud;
            menu.active = 0;
            if (cb) cb(item>=0&&item<menu.n?item:-1,ud);
        } else {
            menu_cancel();
        }
        wm_full();
        return;
    }

    if (launch.active) {
        int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
        if (in_rect(mx, my, px, py, pw, ph)) {
            int hit = launcher_at_point();
            if (hit >= 0) {
                wm_open_app(app_at(hit)->id, NULL);
                launch.active = 0;
            }
            wm_full();
            return;
        }
        if (!in_rect(mx, my, 8, screen_h - TASKBAR_H + 6, 34, 28)) {
            launch.active = 0;
            wm_full();
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
                    if (b == 0) { w->state = WIN_STATE_MIN; wm_full(); }
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
                            w->state = WIN_STATE_MAX; wm_full();
                            w->dirty = 1;
                            win_alloc_buf(w);
                        }
                    } else {
                        wm_close_window(w);
                    }
                    wm_full();
                    return;
                }
            }
            drag_win = w;
            drag_ox = rx; drag_oy = ry;
            wm_full();
            return;
        }
        /* resize handle */
        if (in_rect(rx, ry, w->w - 16, w->h - 16, 16, 16) && w->state != WIN_STATE_MAX) {
            resize_win = w;
            resize_ox = w->w - rx; resize_oy = w->h - ry;
            wm_full();
            return;
        }
        if (w->app && w->app->mouse)
            { APP_T0(w); w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR)); APP_T1(w); }
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
                wm_full();
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
            lua_apps_refresh();
            launch.offset = 0;
            launch.active = !launch.active;
            wm_full();
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
                struct window *tw = wins[i];
                if (tw->state == WIN_STATE_MIN) {
                    tw->state = WIN_STATE_NORMAL;
                    wm_focus(tw);
                } else if (tw == focused_w) {
                    tw->state = WIN_STATE_MIN; wm_full();
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
    /* Ctrl+Alt+Del: the escape hatch firmware used to provide */
    if (e->pressed && e->ctrl && e->alt &&
        (e->keycode == KEY_DELETE || e->keycode == 0x7F)) {
        klog("wm: ctrl+alt+del - rebooting");
        cpu_reboot_8042();
    }
    /* r37: Ctrl+Alt+F1 - the Linux-style console switch.  Hands screen
     * and keyboard to the kernel maintenance tty (Linux's VT1); from
     * there Ctrl+Alt+F7 or the 'wm' command returns to the desktop.
     * Gives the user a rescue console even when no terminal app is
     * open or the WM's app layer is wedged. */
    if (e->pressed && e->ctrl && e->alt && e->keycode >= KEY_F1 && e->keycode <= KEY_F6) {
        tty_request = e->keycode - KEY_F1 + 1;
        return;
    }
    if (menu.active && e->pressed && e->keycode == 27) {
        menu_cancel();
        wm_full();
        return;
    }
    if (launch.active) {
        if (e->pressed && e->keycode == 27) { launch.active = 0; wm_full(); return; }
        if (e->pressed && e->keycode == '\n') {
            int matches = 0;
            for (int i = 0; i < app_count(); i++)
                if (launch_match(i) && matches++ >= launch.offset) {
                    wm_open_app(app_at(i)->id, NULL); break;
                }
            launch.active = 0;
            wm_full();
            return;
        }
        edit_line(launch.q, &launch.qpos, sizeof(launch.q), e);
        launch.offset = 0;
        launch_dirty = 1;
        dirty = 1;
        return;
    }
    if (e->pressed && e->keycode == KEY_DELETE && desk_sel && !focused_w && !modal_w) {
        desk_remove_sel();
        dirty = 1;
        return;
    }
    struct window *w = modal_w ? modal_w : focused_w;
    if (w && w->app && w->app->key) { APP_T0(w); w->app->key(w, e); APP_T1(w); }
    dirty = 1;
}

/* --------------------------------------------------------------- tick ---- */
static void wm_tick(void)
{
    for (int i = 0; i < win_count; i++)
        if (wins[i]->app && wins[i]->app->tick && wins[i]->state != WIN_STATE_MIN)
            { APP_T0((wins[i])); wins[i]->app->tick(wins[i]); APP_T1((wins[i])); }

    /* r39: fold each window's accumulated TSC cycles into a percent
     * once per second (budget = cpu_mhz() * 1e6 ticks, the same PIT-
     * calibrated TSC rate SysMon reports). */
    if (cyc_ok) {
        static u64 previous;
        u64 now=rdtsc();
        if(now-previous>=cpu_tsc_hz){
            u64 budget=now-previous;previous=now;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                app_pct[i] = budget ? (u32)(app_cyc[i] * 100 / budget) : 0;
                if (app_pct[i] > 100) app_pct[i] = 100;
                app_cyc[i] = 0;
            }
        }
    }

    /* clock / blink phase: repaint only the rects that actually show them */
    u64 phase = tick_count / 50;
    if (phase != last_blink_phase) {
        last_blink_phase = phase;
        for (int i = 0; i < win_count; i++)
            if (wins[i]->state != WIN_STATE_MIN) wins[i]->chrome_dirty = 1;
        dirty = 1;
    }
    {
        static u32 last_sec_tick;
        if (tick_count / 100 != last_sec_tick) {
            last_sec_tick = (u32)(tick_count / 100);
            tb_dirty = 1;
            dirty = 1;
        }
    }
    if (launch.active && (tick_count % 50) == 0) {
        launch_dirty = 1;
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
    static u32 last_sec, last_tick, last_wd;
    if ((u32)tick_count - last_wd < 100) return;   /* 1 Hz, tick-based */
    last_wd = (u32)tick_count;
    struct rtc_time rt;
    rtc_read(&rt);
    if (last_sec && rt.sec != last_sec && (u32)tick_count == last_tick) {
        outb(0x20, 0x20);
        outb(0xA0, 0x20);
        pic_clear_mask(0);
        klog("wm: irq watchdog re-armed PIC");
        err_notify("interrupt controller",
                   "IRQ watchdog re-armed the PIC (ticks had stopped)", 0, 0);
    }
    last_sec = rt.sec;
    last_tick = (u32)tick_count;
}

static u32 last_paint_tick;
volatile int wm_in_idle;

void wm_theme_changed(void)
{
    wp_valid = 0;
    wm_full();
}

/* Called only after dispatch has unwound, never from inside an app
 * callback. Terminating the compositor also destroys its dependent apps. */
static void wm_destroy_session(void)
{
    menu_cancel();
    u64 total, before, after; mm_stats(&total,&before);
    pending_console = NULL;
    for (int i=0;i<win_count;i++) wins[i]->console=NULL;
    while (win_count) wm_close_window(wins[win_count-1]);
    focused_w=modal_w=drag_win=resize_win=NULL;
    mouse_rx=mouse_ry=mouse_sens=0;
    rs_pending=0; band_active=0; desk_drag=-1; desk_sel=0;
    menu.active=0; launch.active=0; ndmg=0; mbuttons=0;
    if (wp_cache.px) {
        pfree(wp_cache.px,(u32)wp_cache.w*wp_cache.h*4);
        memset(&wp_cache,0,sizeof(wp_cache));
    }
    wp_valid=0;
    if (cur_buf) pfree(cur_buf,CUR_W*CUR_H*4);
    cur_buf=NULL; cur_have=0;
    cyc_reset_all();
    mm_stats(&total,&after);
    klog("wm: terminated; application windows and compositor caches released (%u KB reclaimed)",
         (u32)(after >= before ? after-before : 0));
}

int wm_stop_requested;

void wm_run(void)
{
    wm_stop_requested = 0;
    wm_full(); /* Restart from the base console must replace its entire scene. */
    input_guard_armed = 1;
    cyc_ok = cpu_mhz() != 0;   /* r39: per-app CPU metering needs TSC */

    klog("wm: entering main loop");
    for (;;) {
        if (wm_stop_requested) goto stopped;
        irq_watchdog();
        if (err_pending()) err_show_pending();
        interrupt_poll();
        usb_poll(); /* foreground only: never drive USB from SIMD-free IRQs */
        struct mouse_event me;
        while (mouse_poll(&me)) handle_mouse(&me);
        resize_flush(0);           /* r36: once-per-frame resize apply */
        if (tty_request) {         /* r36: kernel maintenance console */
            tty_select(tty_request);
            tty_request = 0;
            tty_run(1);            /* returns when the user types 'wm' */
            wm_full();             /* tty repainted both buffers */
        }
        struct key_event ke;
        while (kbd_poll(&ke)) {handle_key(&ke);if(tty_request||wm_stop_requested)break;}
        if (wm_stop_requested) goto stopped;

        static u64 last_tick;
        if (tick_count != last_tick) {
            last_tick = tick_count;
            wm_tick();
        }
        if (dirty && (u32)tick_count - last_paint_tick >= 2) {  /* 50 fps cap */
            last_paint_tick = (u32)tick_count;
            if (full_dirty) { paint_all(); full_dirty = 0; }
            else paint_partial();
            dirty = 0;
        }
        wm_in_idle = 1;
        cpu_hlt();
        wm_in_idle = 0;
    }
stopped:
    wm_destroy_session();
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
        wm_redraw(w);      /* r36: same echo bug class as the terminal -
                            * typed dialog input was invisible until OK */
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
    struct dialog_data *d = palloc_owned(sizeof(*d),(uintptr_t)w);
    if (!d) { wm_close_window(w); if (cb) cb(0,NULL,ud); return; }
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
    wm_full();
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
    struct err_data *d = palloc_owned(sizeof(*d),(uintptr_t)w);
    if (!d) { wm_close_window(w); err_notify("ui",text,NULL,0); return; }
    memset(d,0,sizeof(*d));
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

/* The WM registers its internal clients through the same app interface. */
SCOS_APP(dialog_app, 900);
SCOS_APP(error_app, 901);

void wm_poweroff_screen(void)
{
    fb_clear(0x000000);
    const struct theme *t = theme_current();
    s_text_scaled(&screen, (screen_w - 4 * 8 * 3) / 2, screen_h / 2 - 60, "SCos", t->main, 3);
    const char *m = "It is now safe to turn off your computer.";
    s_text(&screen, (screen_w - s_text_width(m)) / 2, screen_h / 2, m, 0xAAAAAA);
    fb_flip();
}
