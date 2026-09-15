/*
 * SCos native - System Monitor.
 *
 * Everything shown here is measured, not simulated:
 *   - CPU brand from CPUID, speed from a TSC-vs-PIT calibration,
 *   - CPU load from idle-halt time accounted in the PIT interrupt,
 *   - memory from the kernel page allocator,
 *   - the task table is the live window/app table of the WM plus the
 *     always-present kernel services; "End Task" really closes the window.
 */
#include "scos.h"

#define SYS_TASKS 5
static const char *sys_names[SYS_TASKS] = {
    "scos-kernel", "wm-compositor", "vfs+ata-fs", "pit-clock", "ps2-kbd/mouse",
};

struct smon {
    int sel;              /* selected row, -1 = none; >= SYS_TASKS = app */
    int hover_btn;
    int rows;
};

static void sm_tick(struct window *w)
{
    if ((tick_count % 50) == 0) wm_redraw(w);
}

static void sm_paint(struct window *w)
{
    struct smon *m = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    char line[128], n[16];
    int y = 10;
    s_text(s, 12, y, "CPU", t->main); y += 20;
    char cpu[49];
    cpu_brand(cpu, sizeof(cpu));
    strcpy(line, "  model:  "); strcat(line, cpu[0] ? cpu : "x86 processor (no brand leaf)");
    s_text(s, 12, y, line, t->text); y += 18;
    u32 sig = cpu_signature();
    u32 fam = (sig >> 8) & 0xF;
    if (fam == 0xF) fam += (sig >> 20) & 0xFF;
    u32 mod = (sig >> 4) & 0xF;
    if (fam == 6 || fam == 0xF) mod += (sig >> 16) & 0xF;
    strcpy(line, "  cpuid:  family ");
    fmt_u32(n, fam); strcat(line, n);
    strcat(line, " model "); fmt_u32(n, mod); strcat(line, n);
    strcat(line, " stepping "); fmt_u32(n, sig & 0xF); strcat(line, n);
    s_text(s, 12, y, line, t->text); y += 18;
    strcpy(line, "  speed:  ");
    fmt_u32(n, cpu_mhz()); strcat(line, n); strcat(line, " MHz (TSC calibrated against PIT)");
    s_text(s, 12, y, line, t->text); y += 18;
    u32 load = cpu_usage_pct();
    strcpy(line, "  load:   ");
    fmt_u32(n, load); strcat(line, n); strcat(line, "%  (idle-halt accounting, 1s window)");
    s_text(s, 12, y, line, t->text); y += 18;
    int bw = 300;
    s_frame_rect(s, 30, y, bw, 12, t->main);
    s_fill(s, 31, y + 1, (int)((bw - 2) * load) / 100, 10, t->main);
    y += 22;

    u32 tot = 0, fre = 0;
    mm_stats(&tot, &fre);
    s_text(s, 12, y, "Memory", t->main); y += 20;
    strcpy(line, "  used:   ");
    fmt_u32(n, (tot - fre) / 1024); strcat(line, n); strcat(line, " MB of ");
    fmt_u32(n, tot / 1024); strcat(line, n); strcat(line, " MB");
    s_text(s, 12, y, line, t->text); y += 18;
    s_frame_rect(s, 30, y, bw, 12, t->main);
    s_fill(s, 31, y + 1, (int)((bw - 2) * (tot - fre)) / (int)(tot ? tot : 1), 10, t->main);
    y += 22;

    strcpy(line, "Uptime: ");
    fmt_u32(n, uptime_ms() / 1000); strcat(line, n); strcat(line, " s");
    s_text(s, 12, y, line, t->text); y += 24;

    s_text(s, 12, y, "Tasks", t->main); y += 20;
    s_text(s, 16, y, "PID", t->text);
    s_text(s, 60, y, "NAME", t->text);
    s_text(s, 260, y, "TYPE", t->text);
    s_text(s, 360, y, "STATE", t->text);
    s_text(s, 470, y, "MEM", t->text);
    y += 6;
    s_fill(s, 12, y, s->w - 24, 1, t->main);
    y += 6;

    int nwin = wm_win_count();
    m->rows = SYS_TASKS + nwin;
    for (int i = 0; i < m->rows && y < s->h - 46; i++) {
        int rh = 18;
        u32 bg = 0;
        if (i == m->sel) bg = color_blend(t->win_bg, t->main, 30);
        else if (i >= SYS_TASKS && (i - SYS_TASKS) % 2) bg = color_blend(t->win_bg, t->main, 8);
        if (bg) s_fill(s, 12, y - 2, s->w - 24, rh, bg);
        if (i < SYS_TASKS) {
            fmt_u32(n, (u32)i);
            s_text(s, 16, y, n, t->text);
            s_text(s, 60, y, sys_names[i], t->text);
            s_text(s, 260, y, "system", ((t->main >> 1) & 0x7F7F7F));
            s_text(s, 360, y, "running", t->text);
            s_text(s, 470, y, "-", t->text);
        } else {
            struct window *aw = wm_win_at(i - SYS_TASKS);
            fmt_u32(n, (u32)(10 + i - SYS_TASKS));
            s_text(s, 16, y, n, t->text);
            s_text(s, 60, y, aw->app ? aw->app->id : "?", t->text);
            s_text(s, 260, y, "app", t->main);
            s_text(s, 360, y, aw->state == WIN_STATE_MIN ? "minimized" : "running", t->text);
            char mb[24];
            fmt_u32(mb, (u32)(aw->surf.w * aw->surf.h * 4) / 1024);
            strcat(mb, " KB");
            s_text(s, 470, y, mb, t->text);
        }
        y += rh;
    }

    /* End Task button */
    int by = s->h - 36;
    int enabled = m->sel >= SYS_TASKS;
    u32 bg = (m->hover_btn && enabled) ? t->main : 0x222222;
    u32 fg = (m->hover_btn && enabled) ? t->title_text : (enabled ? t->main : 0x666666);
    s_fill(s, 12, by, 110, 26, bg);
    s_frame_rect(s, 12, by, 110, 26, fg);
    s_text(s, 26, by + 5, "End Task", fg);
    s_text(s, 140, by + 6, m->sel >= SYS_TASKS
             ? "click a task row, then End Task (system tasks cannot be ended)"
             : "select an app task to enable End Task",
           ((t->main >> 1) & 0x7F7F7F));
}

static void sm_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct smon *m = w->data;
    struct surface *s = &w->surf;
    int old_sel = m->sel, old_h = m->hover_btn;
    m->hover_btn = (x >= 12 && x < 122 && y >= s->h - 36 && y < s->h - 10);

    /* row geometry mirrors paint (CPU block grew by one line this round) */
    int y0 = 240;
    if (e->type == MEV_BUTTON && e->down && e->button == MBTN_LEFT) {
        if (y >= y0 && y < y0 + m->rows * 18) {
            int row = (y - y0) / 18;
            if (row < m->rows) m->sel = (m->sel == row) ? -1 : row;
        } else if (m->hover_btn && m->sel >= SYS_TASKS) {
            struct window *victim = wm_win_at(m->sel - SYS_TASKS);
            if (victim && victim != w) {
                wm_close_window(victim);
                m->sel = -1;
            } else if (victim == w) {
                m->sel = -1;   /* refusing to saw off our own branch */
            }
        } else if (m->hover_btn) {
            m->sel = -1;
        }
    }
    if (old_sel != m->sel || old_h != m->hover_btn) wm_redraw(w);
}

static void sm_key(struct window *w, struct key_event *e)
{
    struct smon *m = w->data;
    if (!e->pressed) return;
    if (e->keycode == KEY_UP && m->sel > 0) m->sel--;
    else if (e->keycode == KEY_DOWN && m->sel < m->rows - 1) m->sel++;
    else return;
    wm_redraw(w);
}

static void sm_open(struct window *w, void *arg)
{
    (void)arg;
    struct smon *m = palloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->sel = -1;
    m->hover_btn = 0;
    w->data = m;
}

static void sm_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct smon));
}

struct app app_sysmon = {
    .id = "sysmon", .title = "System Monitor", .icon = ICON_CHART, .single = 1,
    .def_w = 640, .def_h = 480,
    .open = sm_open, .paint = sm_paint, .key = sm_key,
    .mouse = sm_mouse, .tick = sm_tick, .close = sm_close,
};
