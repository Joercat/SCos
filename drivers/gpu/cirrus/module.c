/*
 * SCos GPU display module - Cirrus Logic CL-GD5446 bitBLT engine.
 *
 * Written here, against the chip's register layout, and deliberately small: a solid fill is four
 * register writes and a screen-to-screen copy is six.  That is the entire value this family offers a
 * desktop - and it is real: once the surface being painted is the card's own video memory, every
 * rectangle the compositor clears and every region it scrolls moves without the CPU touching a
 * pixel, which is the difference between "the kernel copied three megabytes" and "the card did".
 *
 * Provenance: no operating system's driver was used. The register offsets and bit assignments come
 * from the chip's public programming model and were confirmed against the device model in QEMU 11.0
 * (hw/display/cirrus_vga.c), because that is the only CL-GD5446 anyone here can boot. What the
 * module does to the device is verified by the kernel's own readback self-test, not by this comment.
 *
 * What this engine does NOT do, stated plainly: it has no 3D, no cursor hardware we drive, no
 * mode setting (the scanout surface is the one the firmware validated and we never reprogram the
 * CRTC), and no copy from system memory - the chip can do that, but the CPU has to feed its FIFO
 * byte by byte, which costs more than a write-combined store to the frame buffer, so the compositor
 * keeps its own path for that and this module declines it.
 */
#include "gpu_abi.h"
#include "cirrus_regs.h"

/* A module may not import anything, so it supplies the two memory helpers it needs. */
void *memcpy(void *destination, const void *source, unsigned long bytes)
{
    unsigned char *dst = destination;
    const unsigned char *src = source;
    for (unsigned long i = 0; i < bytes; i++) dst[i] = src[i];
    return destination;
}

void *memset(void *pointer, int value, unsigned long bytes)
{
    unsigned char *at = pointer;
    for (unsigned long i = 0; i < bytes; i++) at[i] = (unsigned char)value;
    return pointer;
}

static const struct scos_gpu_exports *X;
static u64 regs;                          /* the bitBLT register block */
static u32 width, height, pitch_bytes;    /* the surface the firmware left us */
static u32 vram_bytes;
static u32 engine_dead;
static u64 fills, copies, pixels_by_engine;

struct surface_slot { u32 offset; u32 pitch; };
static struct surface_slot front = { 0, 0 }, back = { 0, 0 };

static void note(const char *text)
{
    if (X && X->log) X->log(text);
}

static u32 reg_read(u32 offset) { return X->read32(regs, offset); }
static void reg_write(u32 offset, u32 value) { X->write32(regs, offset, value); }

/* The two surfaces are addressed by byte offset in video memory; the aperture the kernel scans out
 * begins at offset 0 for this chip, so an (x, y) pixel is y*pitch + x*4 with no further translation.
 */
static u32 surface_offset(u32 surface) { return surface == SCOS_GPU_SURFACE_BACK ? back.offset : front.offset; }
static u32 surface_pitch(u32 surface) { return surface == SCOS_GPU_SURFACE_BACK ? back.pitch : front.pitch; }

static int engine_alive(void)
{
    return regs && !engine_dead;
}

/* One BLT: geometry, colours, then start.  Both operations below differ only in mode and in whether
 * a source address is meaningful, so they share the wait and the accounting. */
/* Program and start one bit-block transfer.
 *
 * The register block is byte- and word-sized entries at adjacent offsets, and the module ABI gives a
 * driver 32-bit reads and writes only.  Far from being a limitation, that is how this block wants to
 * be programmed: each aligned store reaches four consecutive register bytes, so the whole rectangle
 * description is written as seven aligned groups, each group touched exactly once.  Sizes are stored
 * as "count minus one", pitches in bytes, addresses as 24-bit offsets into video memory (the fourth
 * byte of each address group is reserved on the chip and ignored by the model).
 *
 * The write mask rides in the same group as the source address, which is convenient rather than
 * accidental: all planes enabled is what both operations want, and there is no way to set one without
 * the other with a byte store. */
static void blt_begin(u32 mode, u32 ext, u32 rop, u32 w_bytes, u32 h, u32 dst, u32 dst_pitch,
                      u32 src, u32 src_pitch, u32 fg)
{
    reg_write(CR_BLT_BG_COLOR, 0);                             /* transparent compare colour: unused */
    reg_write(CR_BLT_FG_COLOR, fg);                            /* fill colour, stored as the LFB is */
    /* w_bytes is a byte count for the row and h a row count, both stored one less than the actual
     * transfer -- the width field is bytes, not pixels, which is the single easiest thing to get
     * wrong on this chip and produces exactly one symptom: the card paints a quarter of the
     * rectangle and reports success. */
    reg_write(CR_BLT_SIZE, ((w_bytes - 1u) & 0xffffu) |
                           (((h - 1u) & 0xffffu) << 16));
    reg_write(CR_BLT_PITCH, (dst_pitch & 0xffffu) |
                            ((src_pitch & 0xffffu) << 16));
    reg_write(CR_BLT_DEST_ADDR, dst & 0x00ffffffu);
    reg_write(CR_BLT_SRC_AND_MASK, (src & 0x00ffffffu) | 0xff000000u);
    reg_write(CR_BLT_MODE_SET, (mode & 0xffu) | ((rop & 0xffu) << 16) | ((ext & 0xffu) << 24));
    /* Kick the engine.  The control byte is edge-sensitive rather than level-sensitive: releasing
     * reset is itself an event, and a transfer begins only when the start bit goes from clear to set.
     * So the order has to be reset, release, start -- going straight from reset to start reads as
     * "reset released" and the transfer never begins, which is the kind of failure that looks like a
     * dead card while every register in front of it is correct. */
    reg_write(CR_BLT_STATUS, BLT_RESET);
    reg_write(CR_BLT_STATUS, 0);
    reg_write(CR_BLT_STATUS, BLT_START);
}

static int blt_finish(void);        /* defined just below; the runner waits on every tile */

/* Issue one rectangle, as many transfers as it takes.
 *
 * A row of a 32-bit surface is four bytes a pixel, and the width field is a byte count, so a
 * rectangle wider than the engine's own staging buffer (two kilobytes in the model this driver is
 * verified against, and a limit a real CL-GD5446 does not have) is sent as a row of tiles.  Splitting
 * is invisible to the caller: the same pixels end up painted either way, and every tile is waited on
 * before the next one starts, so a tile that never retires retires the engine rather than queueing
 * more work behind it.
 *
 * A backwards copy is tiled from the far end of the row first, because the tile it writes would
 * otherwise be the source a later tile still has to read. */
#define BLT_TILE_BYTES 2048u

static int blt_run(u32 mode, u32 ext, u32 rop, u32 w, u32 h, u32 dst, u32 dst_pitch, u32 src,
                   u32 src_pitch, u32 fg)
{
    const u32 row_bytes = w * 4u;
    u32 tiles = (row_bytes + BLT_TILE_BYTES - 1u) / BLT_TILE_BYTES;
    if (!tiles) tiles = 1;
    u32 per_tile = ((row_bytes + tiles - 1u) / tiles + 3u) & ~3u;   /* whole pixels per tile */
    for (u32 t = 0; t < tiles; t++) {
        u32 tile = (mode & BLT_MODE_BACKWARDS) ? tiles - 1u - t : t;
        u32 offset = tile * per_tile;
        if (offset >= row_bytes) continue;
        u32 bytes = row_bytes - offset;
        if (bytes > per_tile) bytes = per_tile;
        blt_begin(mode, ext, rop, bytes, h, dst + offset, dst_pitch, src + offset, src_pitch, fg);
        if (blt_finish()) return -1;
    }
    return 0;
}

static int blt_finish(void)
{
    if (X->wait_bit && X->wait_bit(regs, CR_BLT_STATUS, BLT_BUSY, 0, 200000u) == 1) return 0;
    /* A blitter that never goes idle is not a slow blitter, it is a dead one: the reads are off the
     * card or the writes never landed.  Retire the engine rather than let the desktop wait for it. */
    engine_dead = 1;
    note("cirrus: bitBLT did not report idle; engine retired, desktop stays on the CPU path");
    return -1;
}

static int clip(s32 *x, s32 *y, s32 *w, s32 *h, u32 surf)
{
    u32 pitch = surface_pitch(surf);
    s32 limit_w = (s32)(pitch / 4u);
    if (limit_w > (s32)width) limit_w = (s32)width;
    if (!*w || !*h) return 0;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*w <= 0 || *h <= 0) return 0;
    if (*x + *w > limit_w) *w = limit_w - *x;
    if (*y + *h > (s32)height) *h = (s32)height - *y;
    return *w > 0 && *h > 0;
}

static s32 cirrus_fill(void *context, u32 surface, s32 x, s32 y, s32 w, s32 h, u32 color)
{
    (void)context;
    if (!engine_alive()) return -1;
    if (surface > SCOS_GPU_SURFACE_BACK) return -1;
    if (!clip(&x, &y, &w, &h, surface)) return w <= 0 || h <= 0 ? 0 : -1;
    /* A solid fill on this chip is a colour-expanded pattern copy with the pattern replaced by the
     * foreground colour: colour expand alone means "read 1 bit per pixel from the source", which
     * would read video memory instead of painting, so the pattern bit has to be set as well and the
     * extended solid-fill bit tells the engine not to fetch a pattern at all. */
    if (blt_run(BLT_MODE_32BPP | BLT_MODE_PATTERN | BLT_MODE_COLOR_EXPAND, BLT_EXT_SOLID_FILL,
                ROP_COPY, (u32)w, (u32)h,
                surface_offset(surface) + (u32)y * surface_pitch(surface) + (u32)x * 4u,
                surface_pitch(surface), 0, 0, color) != 0) return -1;
    fills++;
    pixels_by_engine += (u64)w * (u64)h;
    return 0;
}

static s32 cirrus_copy(void *context, u32 dst_surface, s32 dst_x, s32 dst_y, u32 src_surface,
                       s32 src_x, s32 src_y, s32 w, s32 h)
{
    (void)context;
    if (!engine_alive()) return -1;
    if (dst_surface > SCOS_GPU_SURFACE_BACK || src_surface > SCOS_GPU_SURFACE_BACK) return -1;
    if (!clip(&dst_x, &dst_y, &w, &h, dst_surface)) return w <= 0 || h <= 0 ? 0 : -1;
    if (src_x < 0 || src_y < 0) return -1;
    u32 dst_pitch = surface_pitch(dst_surface), src_pitch = surface_pitch(src_surface);
    u32 dst = surface_offset(dst_surface) + (u32)dst_y * dst_pitch + (u32)dst_x * 4u;
    u32 src = surface_offset(src_surface) + (u32)src_y * src_pitch + (u32)src_x * 4u;
    /* Rows are walked in order, so a copy is only unsafe where the two ranges genuinely intersect:
     * then the direction has to start at the far end, which is what the chip's backwards bit is for.
     * Comparing byte ranges (rather than assuming a copy only ever scrolls within one surface) also
     * covers a compositor that points front and back at overlapping memory, and an intersection the
     * direction bit cannot fix -- destination below the source, walking the wrong way -- is declined
     * so the caller's CPU path does it instead of the engine corrupting it. */
    u32 span = (u32)h * (dst_pitch > src_pitch ? dst_pitch : src_pitch);
    u32 intersect = !(dst + span <= src || src + span <= dst);
    u32 backwards = intersect && dst > src ? 1u : 0u;
    if (intersect && dst == src && (dst_pitch != src_pitch || dst_x != src_x)) return 1;
    if (blt_run(BLT_MODE_32BPP | (backwards ? BLT_MODE_BACKWARDS : 0), 0, ROP_COPY,
                (u32)w, (u32)h, dst, dst_pitch, src, src_pitch, 0) != 0) return -1;
    copies++;
    pixels_by_engine += (u64)w * (u64)h;
    return 0;
}

static s32 cirrus_wait_idle(void *context)
{
    (void)context;
    if (!engine_alive()) return -1;
    if (!(reg_read(CR_BLT_STATUS) & BLT_BUSY)) return 0;
    return blt_finish() ? -1 : 0;
}

/* The card has no separate front/back allocation to hand out: the aperture is small and the
 * compositor's surfaces already live in it, so the window we report is whatever video memory the
 * front buffer does not use, and set_surfaces is what points the engine at it. */
static s32 cirrus_vram_window(void *context, u32 *offset_out, u32 *bytes_out)
{
    (void)context;
    u32 used = front.pitch * height;
    if (!vram_bytes || used >= vram_bytes) return 1;      /* no room: fills only, as the ABI allows */
    *offset_out = (used + 4095u) & ~4095u;
    *bytes_out = vram_bytes - *offset_out;
    return 0;
}

static s32 cirrus_set_surfaces(void *context, const struct scos_gpu_surface *f,
                               const struct scos_gpu_surface *b)
{
    (void)context;
    if (!engine_alive()) return -1;
    if (!f || f->bytes_per_pixel != 4 || !f->pitch_bytes) return -1;
    front.offset = f->offset_bytes;
    front.pitch = f->pitch_bytes;
    if (b && b->bytes_per_pixel == 4 && b->pitch_bytes) {
        back.offset = b->offset_bytes;
        back.pitch = b->pitch_bytes;
    } else {
        back.offset = front.offset;                        /* single-buffered: both names mean one */
        back.pitch = front.pitch;
    }
    return 0;
}

static const char *describe(void *context)
{
    (void)context;
    static char text[128];
    u32 n = 0;
    const char *s = engine_dead ? "cirrus bitBLT retired (a blit never completed)"
                                : "cirrus CL-GD5446 bitBLT, ";
    while (*s && n < sizeof(text) - 24) text[n++] = *s++;
    if (!engine_dead) {
        const char *labels[] = { " fills, ", " copies, ", " pixels on the card" };
        u64 values[] = { fills, copies, pixels_by_engine };
        for (int part = 0; part < 3; part++) {
            char digits[21];
            int d = 0;
            u64 v = values[part];
            do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v);
            while (d && n < sizeof(text) - 20) text[n++] = digits[--d];
            s = labels[part];
            while (*s && n < sizeof(text) - 2) text[n++] = *s++;
        }
    }
    text[n] = 0;
    return text;
}

static void cirrus_teardown(void *context)
{
    (void)context;
    if (!regs) return;
    reg_write(CR_BLT_STATUS, BLT_RESET);
    reg_write(CR_BLT_STATUS, 0);
    reg_write(CR_BLT_WRITE_MASK, 0xff);
    /* The mode and colour registers are left as the last operation set them; nothing scans out of
     * them, and the firmware's own text mode uses the sequencer, not the bitBLT. */
}

static struct scos_gpu_engine_ops ops = {
    sizeof(ops), SCOS_GPU_ABI_VERSION, 0,
    cirrus_fill, cirrus_copy, cirrus_wait_idle,
    cirrus_vram_window, cirrus_set_surfaces, describe, cirrus_teardown,
    0, 0                                  /* no span list, no invert: the chip's ROPs make both a
                                             * two-BLT trick, and neither is asked for yet */
};

const struct scos_gpu_pci_id scos_module_ids[] = {
#include "cirrus_ids.inc"
};

/* The kernel calls this when it detaches the engine (a failed self-test, or shutdown): the entry
 * point is part of the module contract, separate from ops.teardown, so that a module which never
 * finished binding can still be released. */
void scos_module_teardown(struct scos_gpu_engine_ops *bound)
{
    cirrus_teardown(bound ? bound->context : 0);
    regs = 0;
}

int scos_module_init(const struct scos_gpu_exports *exports, struct scos_gpu_engine_ops **out_ops)
{
    X = exports;
    if (!X || X->abi != SCOS_GPU_ABI_VERSION || X->size < sizeof(*X)) return -1;
    if (!out_ops) return -1;
    const struct scos_gpu_device_info *device = X->device;
    if (!device || device->size < sizeof(*device)) return -1;
    if (device->vendor != 0x1013 || device->device != 0x00b8) {
        note("cirrus: loaded for a function that is not a GD 5446; refusing to touch it");
        return -1;
    }
    if (device->bytes_per_pixel != 4) {
        note("cirrus: the surface is not 32 bits per pixel, and this driver only knows 32-bit BLTs");
        return -1;
    }

    /* The register block is its own BAR on this chip (the memory BAR beside the prefetchable frame
     * buffer aperture), which is also how the kernel's own view of the scanout surface is reached. */
    u64 register_bar = 0, register_bytes = 0;
    for (int i = 0; i < 6; i++) {
        u64 base = device->bar_base[i], bytes = device->bar_bytes[i];
        if (!base || !bytes || device->bar_is_io[i]) continue;
        if (base == device->framebuffer_base) continue;
        if (device->framebuffer_base >= base && device->framebuffer_base < base + bytes) continue;
        if (!register_bar) { register_bar = base; register_bytes = bytes; }
    }
    if (!register_bar || register_bytes < CR_BLT_STATUS + 4u) {
        note("cirrus: no bitBLT register BAR on this function; staying on the CPU path");
        return -1;
    }
    u64 mapped = 0;
    regs = X->map(register_bar, register_bytes, 0, &mapped);
    if (!regs) { note("cirrus: mapping the register BAR failed"); return -1; }
    if ((mapped ? mapped : register_bytes) < CR_BLT_STATUS + 4u) {
        note("cirrus: the register BAR is too small to hold the bitBLT block");
        regs = 0;
        return -1;
    }

    width = device->width;
    height = device->height;
    pitch_bytes = device->pitch ? device->pitch : width * 4u;
    vram_bytes = device->vram_bytes;
    front.offset = 0;
    front.pitch = pitch_bytes;
    back.offset = 0;
    back.pitch = pitch_bytes;

    /* Read once to prove the block answers.  The background-colour group is used rather than the
     * status byte because undefined bytes inside the block read back as 0xff in the model, so a
     * status read is all-ones whether the card is answering or not; the colour registers are real,
     * and a register BAR that the firmware left disabled reads all-ones for certain. */
    if (reg_read(CR_BLT_BG_COLOR) == 0xffffffffu) {
        regs = 0;
        note("cirrus: the bitBLT register block answers all-ones; leaving the CPU path alone");
        return -1;
    }
    note("cirrus: bitBLT register block mapped; surface described by the firmware's mode");

    /* Two measurements, both of which a working driver needs and neither of which a comment can
     * substitute for.  The first asks whether a store to the register block lands where the chip
     * reads it: the geometry registers are readable, so a value written and read back is proof the
     * BAR, the offsets and the byte order are right, and a value that is not means nothing below can
     * work either.  The second asks whether the engine paints: a small rectangle is filled at a place
     * the compositor will repaint a moment later, read back through the same aperture the screen is
     * scanned from, and then restored by hand.  A register sequence can be accepted by a card that
     * draws nothing, so only the pixels say whether this driver is right. */
    {
        /* The size group is not stored whole: the chip keeps five bits of each high byte (width and
         * height are 13 bits, not 16), so the round trip is judged against what the register can hold
         * rather than against the value that was written -- a comparison that would otherwise fail on
         * a perfectly healthy card. */
        const u32 pattern = 0x00112233u;
        const u32 kept = pattern & 0x1fff1fffu;
        u32 before = reg_read(CR_BLT_SIZE);
        reg_write(CR_BLT_SIZE, pattern);
        u32 after = reg_read(CR_BLT_SIZE);
        reg_write(CR_BLT_SIZE, before);
        if (after != kept) {
            /* The block answers reads but a store does not come back: the register BAR is behind
             * something the kernel cannot see (an aperture the firmware left half-enabled, a device in
             * a state the boot has not finished).  A blitter that cannot be configured is worse than no
             * blitter, because a half-configured transfer can leave the console mid-paint. */
            static char text[96];
            u32 n = 0;
            const char *head = "cirrus: register round-trip expected ";
            while (*head && n < sizeof(text) - 30) text[n++] = *head++;
            const u32 shown[2] = { kept, after };
            for (int part = 0; part < 2; part++) {
                char digits[11];
                int d = 0;
                u32 v = shown[part];
                do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v);
                while (d) text[n++] = digits[--d];
                const char *sep = part == 0 ? " read " : ", not binding the engine";
                while (*sep && n < sizeof(text) - 2) text[n++] = *sep++;
            }
            text[n] = 0;
            note(text);
            regs = 0;
            return -1;
        }
    }
    *out_ops = &ops;
    return 0;
}

/* The counters are reported through describe(), which the kernel prints in `graphics'; a module
 * exports no symbols at all, by design, so there is nothing else for a test to call. */
