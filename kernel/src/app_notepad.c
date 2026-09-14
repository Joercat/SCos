/* SCos native - Notepad text editor app */
#include "scos.h"

struct notepad {
    char *text;
    u32 len, cap;
    u32 pos;
    int scroll;             /* first visible line */
    char filepath[256];
    int has_path;
    int hover_btn;          /* 0 save, 1 save as */
};

#define NP_TOOL_H 34

static void np_open(struct window *w, void *arg)
{
    struct notepad *np = palloc(sizeof(*np));
    memset(np, 0, sizeof(*np));
    np->cap = 4096;
    np->text = palloc(np->cap);
    np->text[0] = 0;
    np->hover_btn = -1;
    np->scroll = 0;
    if (arg) {
        const char *path = arg;
        u32 len = 0;
        char *data = vfs_read(path, &len);
        if (data) {
            if (len + 1 > np->cap) { pfree(np->text, np->cap); np->cap = len + 1024; np->text = palloc(np->cap); }
            memcpy(np->text, data, len);
            np->text[len] = 0;
            np->len = len;
            strncpy(np->filepath, path, sizeof(np->filepath) - 1);
            np->has_path = 1;
            const char *base = path + strlen(path);
            while (base > path && *(base - 1) != '/') base--;
            char title[96];
            strcpy(title, "Notepad - ");
            strcat(title, base);
            wm_set_title(w, title);
        }
    }
    np->pos = np->len;
    w->data = np;
}

static void np_close(struct window *w)
{
    struct notepad *np = w->data;
    if (np) {
        if (np->text) pfree(np->text, np->cap);
        pfree(np, sizeof(*np));
    }
    w->data = NULL;
}

static void np_insert(struct notepad *np, char c)
{
    if (np->len + 2 > np->cap) {
        u32 ncap = np->cap * 2;
        char *nt = palloc(ncap);
        memcpy(nt, np->text, np->len);
        pfree(np->text, np->cap);
        np->text = nt;
        np->cap = ncap;
    }
    for (u32 i = np->len; i > np->pos; i--) np->text[i] = np->text[i - 1];
    np->text[np->pos++] = c;
    np->text[++np->len] = 0;
}

static void np_delete_before(struct notepad *np)
{
    if (!np->pos) return;
    for (u32 i = np->pos; i <= np->len; i++) np->text[i - 1] = np->text[i];
    np->pos--;
    np->len--;
}

static void np_delete_at(struct notepad *np)
{
    if (np->pos >= np->len) return;
    for (u32 i = np->pos; i <= np->len; i++) np->text[i] = np->text[i + 1];
    np->len--;
}

/* line helpers */
static u32 line_start(struct notepad *np, u32 pos)
{
    while (pos > 0 && np->text[pos - 1] != '\n') pos--;
    return pos;
}

static void np_move_line(struct notepad *np, int dir)
{
    u32 ls = line_start(np, np->pos);
    u32 col = np->pos - ls;
    if (dir < 0) {
        if (!ls) { np->pos = 0; return; }
        u32 prev_end = ls - 1;
        u32 pls = line_start(np, prev_end);
        np->pos = pls + col;
        if (np->pos > prev_end) np->pos = prev_end;
    } else {
        u32 le = np->pos;
        while (le < np->len && np->text[le] != '\n') le++;
        if (le >= np->len) { np->pos = np->len; return; }
        u32 nls = le + 1;
        u32 nle = nls;
        while (nle < np->len && np->text[nle] != '\n') nle++;
        np->pos = nls + col;
        if (np->pos > nle) np->pos = nle;
    }
}

static void saved_cb(int ok, const char *text, void *ud)
{
    (void)ok; (void)text; (void)ud;
}

struct np_save_ctx { struct window *w; };

static void save_as_cb(int ok, const char *text, void *ud)
{
    struct np_save_ctx *ctx = ud;
    struct window *w = ctx->w;
    pfree(ctx, sizeof(*ctx));
    if (!ok || !text || !w->data) return;
    struct notepad *np = w->data;
    vfs_write(text, np->text, np->len);
    strncpy(np->filepath, text, sizeof(np->filepath) - 1);
    np->has_path = 1;
    const char *base = text + strlen(text);
    while (base > text && *(base - 1) != '/') base--;
    char title[96];
    strcpy(title, "Notepad - ");
    strcat(title, base);
    wm_set_title(w, title);
    wm_dialog("Notepad", "File saved successfully!", NULL, saved_cb, NULL);
}

static void np_save(struct window *w, int as)
{
    struct notepad *np = w->data;
    if (!as && np->has_path) {
        vfs_write(np->filepath, np->text, np->len);
        wm_dialog("Notepad", "File saved successfully!", NULL, saved_cb, NULL);
        return;
    }
    struct np_save_ctx *ctx = palloc(sizeof(*ctx));
    ctx->w = w;
    wm_dialog("Save As", "Enter file path to save:",
              np->has_path ? np->filepath : "home/documents/untitled.txt",
              save_as_cb, ctx);
}

static void np_paint(struct window *w)
{
    struct notepad *np = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    /* toolbar */
    const char *labels[2] = { "Save", "Save As" };
    for (int b = 0; b < 2; b++) {
        int bx = 8 + b * 90;
        u32 bg = np->hover_btn == b ? t->main : 0x333333;
        s_fill(s, bx, 5, 80, 24, bg);
        s_frame_rect(s, bx, 5, 80, 24, t->main);
        s_text(s, bx + (80 - s_text_width(labels[b])) / 2, 9, labels[b],
               np->hover_btn == b ? t->title_text : t->main);
    }

    /* text */
    int rows = (s->h - NP_TOOL_H - 8) / (FONT_H + 2);
    /* find first visible line offset */
    u32 off = 0;
    int line_no = 0;
    while (line_no < np->scroll && off <= np->len) {
        while (off <= np->len && np->text[off] != '\n') off++;
        off++;
        line_no++;
    }
    int y = NP_TOOL_H + 4;
    u32 p = off;
    char linebuf[256];
    for (int r = 0; r < rows && p <= np->len; r++, y += FONT_H + 2) {
        u32 le = p;
        while (le < np->len && np->text[le] != '\n') le++;
        int llen = (int)(le - p);
        if (llen > 250) llen = 250;
        memcpy(linebuf, np->text + p, llen);
        linebuf[llen] = 0;
        s_text(s, 6, y, linebuf, t->text);
        /* caret */
        if (np->pos >= p && np->pos <= le) {
            int col = (int)(np->pos - p);
            if ((tick_count / 50) % 2 == 0)
                s_fill(s, 6 + col * FONT_W, y, 2, FONT_H, t->main);
        }
        p = le + 1;
        if (le >= np->len && np->text[np->len] != '\n') break;
    }
}

static void np_key(struct window *w, struct key_event *e)
{
    struct notepad *np = w->data;
    if (!e->pressed) return;
    if (e->ctrl && (e->keycode == 's' || e->keycode == 'S')) { np_save(w, 0); return; }
    switch (e->keycode) {
    case '\n': np_insert(np, '\n'); break;
    case '\b': np_delete_before(np); break;
    case KEY_DELETE: np_delete_at(np); break;
    case KEY_LEFT: if (np->pos) np->pos--; break;
    case KEY_RIGHT: if (np->pos < np->len) np->pos++; break;
    case KEY_HOME: np->pos = line_start(np, np->pos); break;
    case KEY_END:
        while (np->pos < np->len && np->text[np->pos] != '\n') np->pos++;
        break;
    case KEY_UP: np_move_line(np, -1); break;
    case KEY_DOWN: np_move_line(np, 1); break;
    default:
        if (e->keycode >= 32 && e->keycode < 127) np_insert(np, (char)e->keycode);
        break;
    }
    /* keep caret visible */
    u32 ls = line_start(np, np->pos);
    int line_no = 0;
    for (u32 i = 0; i < ls; i++) if (np->text[i] == '\n') line_no++;
    int rows = (w->surf.h - NP_TOOL_H - 8) / (FONT_H + 2);
    if (line_no < np->scroll) np->scroll = line_no;
    if (line_no >= np->scroll + rows) np->scroll = line_no - rows + 1;
    wm_redraw(w);
}

static void np_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct notepad *np = w->data;
    int old = np->hover_btn;
    np->hover_btn = -1;
    for (int b = 0; b < 2; b++)
        if (x >= 8 + b * 90 && x < 88 + b * 90 && y >= 5 && y < 29) np->hover_btn = b;
    if (old != np->hover_btn) wm_redraw(w);
    if (e->type == MEV_WHEEL) {
        np->scroll -= e->wheel;
        if (np->scroll < 0) np->scroll = 0;
        wm_redraw(w);
        return;
    }
    if (e->type == MEV_BUTTON && e->down && e->button == MBTN_LEFT && np->hover_btn >= 0)
        np_save(w, np->hover_btn == 1);
}

struct app app_notepad = {
    .id = "notepad", .title = "Notepad", .icon = ICON_NOTEPAD, .single = 0,
    .def_w = 700, .def_h = 500,
    .open = np_open, .paint = np_paint, .key = np_key,
    .mouse = np_mouse, .close = np_close,
};
