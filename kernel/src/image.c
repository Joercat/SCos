#include "scos.h"

static int image_is_space(u8 c)
{
    return c == ' '  ||
    c == '\t' ||
    c == '\r' ||
    c == '\n' ||
    c == '\f' ||
    c == '\v';
}

static u32 ppm_skip_space_comments(const u8 *data, u32 size, u32 pos)
{
    for (;;) {
        while (pos < size && image_is_space(data[pos]))
            pos++;

        if (pos < size && data[pos] == '#') {
            while (pos < size &&
                data[pos] != '\n' &&
                data[pos] != '\r')
                pos++;

            continue;
        }

        break;
    }

    return pos;
}

static int ppm_read_uint(const u8 *data,
                         u32 size,
                         u32 *pos,
                         u32 *value)
{
    u32 p;
    u32 v;
    int have_digit;

    p = ppm_skip_space_comments(data, size, *pos);

    v = 0;
    have_digit = 0;

    while (p < size) {
        u8 c = data[p];

        if (c < '0' || c > '9')
            break;

        have_digit = 1;

        if (v > 429496729UL / 10UL)
            return 0;

        v *= 10;

        if (v > 429496729UL - (u32)(c - '0'))
            return 0;

        v += (u32)(c - '0');
        p++;
    }

    if (!have_digit)
        return 0;

    *pos = p;
    *value = v;

    return 1;
}

static struct surface *ppm_load(const u8 *data, u32 size)
{
    struct surface *img;
    u32 pos;
    u32 width;
    u32 height;
    u32 maxval;
    u64 pixel_count;
    u64 pixel_bytes;
    u32 i;

    if (!data || size < 3)
        return 0;

    if (data[0] != 'P' || data[1] != '6')
        return 0;

    if (!image_is_space(data[2]))
        return 0;

    pos = 2;

    if (!ppm_read_uint(data, size, &pos, &width))
        return 0;

    if (!ppm_read_uint(data, size, &pos, &height))
        return 0;

    if (!ppm_read_uint(data, size, &pos, &maxval))
        return 0;

    if (maxval != 255)
        return 0;

    if (pos >= size || !image_is_space(data[pos]))
        return 0;

    pos++;

    if (width == 0 || height == 0)
        return 0;

    pixel_count = (u64)width * (u64)height;

    if (pixel_count > 0xFFFFFFFFULL / 4ULL)
        return 0;

    pixel_bytes = pixel_count * 3ULL;

    if (pixel_bytes > (u64)(size - pos))
        return 0;

    img = (struct surface *)palloc(sizeof(struct surface));

    if (!img)
        return 0;

    img->px = (u32 *)palloc((u32)(pixel_count * 4ULL));

    if (!img->px) {
        pfree(img, sizeof(struct surface));
        return 0;
    }

    img->w = (int)width;
    img->h = (int)height;

    for (i = 0; i < (u32)pixel_count; i++) {
        u32 r;
        u32 g;
        u32 b;

        r = data[pos++];
        g = data[pos++];
        b = data[pos++];

        img->px[i] =
        (r << 16) |
        (g << 8)  |
        b;
    }

    return img;
}

struct surface *image_load(const void *data, u32 size)
{
    const u8 *p;

    if (!data || size < 2)
        return 0;

    p = (const u8 *)data;

    if (p[0] == 'P' && p[1] == '6')
        return ppm_load(p, size);

    return 0;
}

void image_free(struct surface *img)
{
    u64 bytes;

    if (!img)
        return;

    if (img->px) {
        bytes = (u64)(u32)img->w *
        (u64)(u32)img->h *
        4ULL;

        if (bytes <= 0xFFFFFFFFULL)
            pfree(img->px, (u32)bytes);
    }

    pfree(img, sizeof(struct surface));
}
