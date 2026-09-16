#include "scos.h"

struct image_app {
    char path[256];
    struct surface *img;
};

static void image_unload(struct image_app *ia)
{
    if (ia->img) {
        image_free(ia->img);
        ia->img = 0;
    }
}

static void image_open(struct window *w, void *arg)
{
    struct image_app *ia = palloc(sizeof(struct image_app));

    if (!ia) {
        w->data = NULL;
        return;
    }

    memset(ia, 0, sizeof(*ia));

    if (arg) {
        strncpy(ia->path, (const char *)arg, sizeof(ia->path) - 1);
        ia->path[sizeof(ia->path) - 1] = 0;
    }

    if (ia->path[0]) {
        u32 len;
        const void *data = vfs_read(ia->path, &len);

        if (data && len)
            ia->img = image_load(data, len);
    }

    w->data = ia;
}

static void image_close(struct window *w)
{
    struct image_app *ia = w->data;

    if (!ia)
        return;

    image_unload(ia);
    pfree(ia, sizeof(*ia));
    w->data = NULL;
}

static void image_paint(struct window *w)
{
    struct image_app *ia = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();

    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    if (!ia || !ia->img) {
        const char *msg = "Unable to load image";
        int x = (s->w - s_text_width(msg)) / 2;

        s_text(s, x, s->h / 2 - 10, msg, t->text);

        if (ia && ia->path[0]) {
            x = (s->w - s_text_width(ia->path)) / 2;
            s_text(s, x, s->h / 2 + 12, ia->path, t->main);
        }

        return;
    }

    int x = (s->w - ia->img->w) / 2;
    int y = (s->h - ia->img->h) / 2;

    s_blit(s, ia->img, x, y);

    char line[80];
    char n[16];

    strcpy(line, "PPM  ");

    fmt_u32(n, (u32)ia->img->w);
    strcat(line, n);

    strcat(line, "x");

    fmt_u32(n, (u32)ia->img->h);
    strcat(line, n);

    s_text(s, 8, s->h - 20, line, t->text);
}

static void image_key(struct window *w, struct key_event *e)
{
    (void)w;
    (void)e;
}

static void image_mouse(struct window *w,
                        struct mouse_event *e,
                        int x,
                        int y)
{
    (void)w;
    (void)e;
    (void)x;
    (void)y;
}

struct app app_image = {
    .id = "image",
    .title = "Image Viewer",
    .icon = ICON_IMAGE,
    .single = 1,

    .def_w = 640,
    .def_h = 480,

    .open = image_open,
    .paint = image_paint,
    .key = image_key,
    .mouse = image_mouse,
    .close = image_close,
};
