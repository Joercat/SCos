/* SCos native - Files explorer app (toolbar, navigation, context menus) */
#include "scos.h"

struct files {
    char path[256];
    char names[64][VFS_NAME];
    int n;
    int hover_row;          /* -1 = none */
    int hover_btn;
    u8 synth[64];           /* 0 = real entry, 1 = app shortcut, 2 = pin */
    int notice;             /* 1 = show "hidden system dir" notice */
    u64 last_down_tick;     /* double-click detection */
    int last_down_row;
    char targets[64][192]; /* stable app IDs or pinned paths, never parsed labels */
    int scroll;             /* r36: first visible row (wheel scrolling) */
};

#define TB_Y 8
#define TB_H 26
#define ROW_H 24
#define LIST_Y 44

static int fr_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static u32 fr_mix(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (((ar * (255 - t) + br * t) / 255) << 16) |
           (((ag * (255 - t) + bg * t) / 255) << 8) |
           (((ab * (255 - t) + bb * t) / 255));
}

static void files_load(struct files *f)
{
    for (int i = 0; i < 64; i++) f->synth[i] = 0;
    f->notice = 0;f->last_down_row=-1;f->last_down_tick=0;f->hover_row=-1;
    memset(f->targets,0,sizeof(f->targets));
    f->scroll = 0;              /* r36: new directory starts at the top */
    /* Keep system files terminal-only, including aliases through ../. */
    struct vfs_node *dir = vfs_lookup(f->path);
    struct vfs_node *system=vfs_lookup("/system");
    for(struct vfs_node *n=dir;n;n=n->parent)if(n==system){f->notice=1;f->n=0;return;}
    if(!dir||!dir->is_dir){f->notice=2;f->n=0;return;}
    f->n = vfs_list(dir, f->names, 64);
    /* the desktop folder mirrors the desktop: app shortcuts + pins */
    if (dir == vfs_lookup("home/desktop")) {
        int cnt = wm_desk_vis_count();
        for (int i = 0; i < cnt && f->n < 64; i++) {
            char app[32], path[192], label[40];
            int kind = 0;
            if (!wm_desk_vis_get(i, app, path, label, &kind)) continue;
            strncpy(f->names[f->n],label,VFS_NAME-1);f->names[f->n][VFS_NAME-1]=0;
            strncpy(f->targets[f->n],kind==0?app:path,191);
            f->synth[f->n]=(u8)(kind+1);
            f->n++;
        }
    }
}

static void files_open(struct window *w, void *arg)
{
    struct files *f = palloc(sizeof(struct files));
    if (!f) return;              /* OOM: wm_open_app reports it centrally */
    memset(f, 0, sizeof(*f));
    if (arg) strncpy(f->path, (char *)arg, sizeof(f->path) - 1);
    else strcpy(f->path, "/");
    if(!f->path[0])strcpy(f->path,"/");
    f->hover_row = -1;
    f->hover_btn = -1;
    files_load(f);
    w->data = f;
    wm_track_mem(w, (int)sizeof(*f));
    {
        char lg[260];
        strcpy(lg, "browsing ");
        strncat(lg, f->path, 240);
        app_log(w, lg);
    }
}

static void files_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct files));
    w->data = NULL;
}

static int files_is_dir_row(struct files *f, int row)
{
    struct vfs_node *dir = vfs_lookup(f->path);
    if (!dir) return 0;
    for (struct vfs_node *c = dir->child; c; c = c->sibling)
        if (!strcmp(c->name, f->names[row])) return c->is_dir;
    return 0;
}

static int files_full_path(struct files *f, int row, char *out)
{
    if(row<0||row>=f->n||strlen(f->path)+strlen(f->names[row])+2>=256)return 0;
    strcpy(out, f->path);
    if (out[strlen(out) - 1] != '/') strcat(out, "/");
    strcat(out, f->names[row]);
    return 1;
}

static void files_open_row(struct window *w,int row){
    struct files *f=w->data;if(row<0||row>=f->n)return;
    if(f->synth[row]==1){if(!wm_open_app(f->targets[row],NULL))wm_notify("Application unavailable","This shortcut no longer resolves to an installed application.",1);return;}
    if(f->synth[row]==2){struct vfs_node *n=vfs_lookup(f->targets[row]);if(n&&n->is_dir)wm_open_app("files",f->targets[row]);else if(n)app_open_document(f->targets[row]);else wm_notify("Missing file","The pinned file no longer exists.",1);return;}
    char full[256];if(!files_full_path(f,row,full)){wm_notify("Files","Path too long.",1);return;}
    if(files_is_dir_row(f,row)){strcpy(f->path,full);files_load(f);wm_redraw(w);}else app_open_document(full);
}

/* ------------------------------------------------------- context menus --- */
static const char *menu_file_items[] = { "Open", "Delete", "Pin to Desktop" };
static const char *menu_bg_items[] = { "New Folder", "Refresh" };

struct fm_ctx { struct window *w; int row,id; };

static struct window *files_dialog_win;
static int files_dialog_id;

static void newfolder_cb(int ok, const char *text, void *ud)
{
    (void)ud;
    if (!ok || !text || !files_dialog_win || !files_dialog_win->data || files_dialog_win->id!=files_dialog_id) return;
    struct files *f = files_dialog_win->data;
    if(f->notice){wm_error_popup("/system is terminal-only.");return;}
    char full[300];
    strcpy(full, f->path);
    if (full[strlen(full) - 1] != '/') strcat(full, "/");
    if(strlen(full)+strlen(text)>=sizeof(full)||!text[0]){wm_error_popup("Folder path too long or empty.");return;}
    strcat(full, text);
    if(!vfs_mkdir(full)){wm_error_popup("Cannot create folder: path, permissions or memory.");return;}
    {
        char lg[320];
        strcpy(lg, "created folder ");
        strncat(lg, full, 300);
        app_log(files_dialog_win, lg);
    }
    files_load(f);
    wm_redraw(files_dialog_win);
}

static void file_menu_cb(int item, void *ud)
{
    struct fm_ctx *ctx = ud;
    struct window *w = ctx->w;
    if(item<0||w->id!=ctx->id||!w->data){pfree(ctx,sizeof(*ctx));return;}
    struct files *f = w->data;
    if (item == 0) {
        files_open_row(w,ctx->row);
        pfree(ctx, sizeof(*ctx));
    } else if (item == 2) {
        char full[300];
        if(!files_full_path(f,ctx->row,full)){pfree(ctx,sizeof(*ctx));wm_error_popup("Path too long.");return;}
        if (files_is_dir_row(f, ctx->row)) strcat(full, "/");
        wm_desktop_pin_file(full);
        pfree(ctx, sizeof(*ctx));
    } else {
        pfree(ctx,sizeof(*ctx));
        wm_dialog("Files","Delete files with rm -s in the terminal.",NULL,NULL,NULL);
    }
    wm_redraw(w);
}

static void bg_menu_cb(int item, void *ud)
{
    struct fm_ctx *ctx = ud;
    struct window *w=ctx->w;
    if(item<0||w->id!=ctx->id||!w->data){pfree(ctx,sizeof(*ctx));return;}
    struct files *f = w->data;
    if (item == 0) {
        files_dialog_win = ctx->w;files_dialog_id=ctx->w->id;
        wm_dialog("New Folder", "Enter folder name:", "new folder", newfolder_cb, NULL);
    } else {
        files_load(f);
    }
    pfree(ctx, sizeof(*ctx));
    wm_redraw(w);
}

/* -------------------------------------------------------------- paint ---- */
static void files_paint(struct window *w)
{
    struct files *f = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);


    /* toolbar buttons: back, up, refresh, new folder */
    for (int b = 0; b < 4; b++) {
        int bx = 8 + b * 34;
        u32 bg = f->hover_btn == b ? t->main : color_blend(t->win_bg,t->main,15);
        u32 fg = f->hover_btn == b ? t->title_text : t->main;
        s_fill(s, bx, TB_Y, 30, TB_H, bg);
        s_frame_rect(s, bx, TB_Y, 30, TB_H, t->main);
        if (b == 0) {
            s_line(s, bx + 9, TB_Y + 13, bx + 21, TB_Y + 13, fg);
            s_line(s, bx + 9, TB_Y + 13, bx + 14, TB_Y + 8, fg);
            s_line(s, bx + 9, TB_Y + 13, bx + 14, TB_Y + 18, fg);
        } else if (b == 1) {
            s_line(s, bx + 15, TB_Y + 7, bx + 15, TB_Y + 19, fg);
            s_line(s, bx + 15, TB_Y + 7, bx + 10, TB_Y + 12, fg);
            s_line(s, bx + 15, TB_Y + 7, bx + 20, TB_Y + 12, fg);
        } else if (b == 2) {
            s_circle(s, bx + 15, TB_Y + 13, 6, fg);
            s_fill(s, bx + 16, TB_Y + 4, 8, 4, bg);
            s_line(s, bx + 15, TB_Y + 7, bx + 22, TB_Y + 8, fg);
        } else {
            s_fill(s, bx + 7, TB_Y + 8, 7, 3, fg);
            s_frame_rect(s, bx + 7, TB_Y + 10, 16, 10, fg);
        }
    }
    /* path box */
    int px = 8 + 4 * 34 + 6;
    s_fill(s, px, TB_Y, s->w - px - 8, TB_H, color_blend(t->win_bg,t->main,15));
    s_frame_rect(s, px, TB_Y, s->w - px - 8, TB_H, t->main);
    s_clip_text(s, px + 6, TB_Y + 5, f->path, t->main, s->w - px - 20);

    if(f->notice){s_clip_text(s,12,LIST_Y+10,f->notice==1?"/system is terminal-only.":"Directory unavailable. Go up or refresh.",t->text,s->w-24);return;}
    /* entries (r36: scrollable - rows past the clip used to be
     * unreachable: no wheel handler, no offset, silently invisible) */
    int rows = (s->h - LIST_Y - 6) / ROW_H;
    int maxs = f->n - rows;
    if (maxs < 0) maxs = 0;
    if (f->scroll > maxs) f->scroll = maxs;
    if (f->scroll < 0) f->scroll = 0;
    for (int i = f->scroll; i < f->n && i < f->scroll + rows; i++) {
        int y = LIST_Y + 4 + (i - f->scroll) * ROW_H;
        if (i == f->hover_row)
            s_fill(s, 4, y, s->w - 8, ROW_H, fr_mix(t->win_bg, t->main, 18));
        if (files_is_dir_row(f, i)) {
            s_fill(s, 12, y + 6, 6, 3, t->main);
            s_frame_rect(s, 12, y + 8, 14, 9, t->main);
        } else {
            s_frame_rect(s, 13, y + 4, 11, 15, t->main);
            s_fill(s, 15, y + 8, 7, 1, t->main);
            s_fill(s, 15, y + 11, 7, 1, t->main);
        }
        if (f->synth[i])                     /* shortcut badge */
            s_fill(s, 21, y + 15, 5, 5, t->main);
        s_clip_text(s,34,y+4,f->names[i],t->text,s->w-126);
        s_clip_text(s,s->w-84,y+4,f->synth[i]==1?"App":f->synth[i]==2?"Shortcut":files_is_dir_row(f,i)?"Folder":"File",t->main,76);
    }
    if (!f->n)
        s_text(s, 12, LIST_Y + 10, "(empty directory)", t->text);
}

/* -------------------------------------------------------------- input ---- */
static void files_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct files *f = w->data;
    /* r36: wheel scrolls the entry list (3 rows per notch) */
    if (e->type == MEV_WHEEL) {
        int rows = (w->surf.h - LIST_Y - 6) / ROW_H;
        int maxs = f->n - rows;
        if (maxs < 0) maxs = 0;
        int ns = f->scroll - (int)e->wheel * 3;
        if (ns < 0) ns = 0;
        if (ns > maxs) ns = maxs;
        if (ns != f->scroll) { f->scroll = ns; wm_redraw(w); }
        return;
    }
    int old_btn = f->hover_btn, old_row = f->hover_row;
    f->hover_btn = -1;
    f->hover_row = -1;
    if (y < LIST_Y) {
        for (int b = 0; b < 4; b++)
            if (fr_in(x, y, 8 + b * 34, TB_Y, 30, TB_H)) f->hover_btn = b;
    } else if(x>=4&&x<w->surf.w-4&&y>=LIST_Y+4&&!f->notice) {
        int rows = (w->surf.h - LIST_Y - 6) / ROW_H;
        int row = f->scroll + (y - LIST_Y - 4) / ROW_H;
        /* r36: hover must ignore rows below the visible clip (the old
         * hit test selected INVISIBLE rows whenever n > rows) */
        if (row >= f->scroll && row < f->n && row < f->scroll + rows)
            f->hover_row = row;
    }
    if (f->hover_btn != old_btn || f->hover_row != old_row) wm_redraw(w);

    if (e->type != MEV_BUTTON || !e->down) return;

    if (e->button == MBTN_RIGHT) {

        struct fm_ctx *ctx = palloc(sizeof(*ctx));
        if(!ctx){err_notify("files","Not enough memory for menu.",NULL,0);return;}
        ctx->id=w->id;ctx->w = w;
        ctx->row = f->hover_row;
        if (f->hover_row >= 0)
            wm_menu(mx_abs(w, x), my_abs(w, y), menu_file_items, f->synth[f->hover_row]?1:3, file_menu_cb, ctx);
        else
            wm_menu(mx_abs(w, x), my_abs(w, y), menu_bg_items, 2, bg_menu_cb, ctx);
        return;
    }
    if (e->button != MBTN_LEFT) return;

    if (f->hover_btn >= 0) {
        switch (f->hover_btn) {
        case 0:
        case 1: {
            char tmp[256];
            strcpy(tmp, f->path);
            int len = (int)strlen(tmp);
            if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
            char *slash = NULL;
            for (char *p = tmp; *p; p++) if (*p == '/') slash = p;
            if (slash) { slash[1] = 0; strcpy(f->path, tmp); }
            else strcpy(f->path, "/");
            files_load(f);
            break;
        }
        case 2:
            files_load(f);
            break;
        case 3:
            files_dialog_win = w;files_dialog_id=w->id;
            wm_dialog("New Folder", "Enter folder name:", "new folder", newfolder_cb, NULL);
            break;
        }
        wm_redraw(w);
        return;
    }
    if (f->hover_row >= 0) {
        int row = f->hover_row;
        u64 now = tick_count;
        int dbl = (row == f->last_down_row &&
                   (now - f->last_down_tick) <= (u64)prefs_get()->dbl_ms / 10);
        f->last_down_tick = now;
        f->last_down_row = row;
        if (!dbl) { wm_redraw(w); return; }   /* first click selects */
        files_open_row(w,row);
        wm_redraw(w);
    }
}

static void files_key(struct window *w, struct key_event *e)
{
    if(e->pressed&&e->keycode==KEY_F5){files_load(w->data);wm_redraw(w);}
}

struct app app_files = {
    .desktop_label = "Files",
    .uses_data = 1, .id = "files", .title = "File Explorer", .icon = ICON_FOLDER, .single = 0,
    .def_w = 700, .def_h = 500,
    .open = files_open, .paint = files_paint, .key = files_key,
    .mouse = files_mouse, .close = files_close,
};

SCOS_APP(app_files, 000);
