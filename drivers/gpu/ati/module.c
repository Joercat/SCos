/* SCos GPU display module: ATI Rage 128 2D engine.
 *
 * What this is: the first real per-family driver module in SCos.  It is not written from scratch
 * and it is not a stub.  The register programming below is ported function by function from
 * Haiku 7be0fef07df0 src/add-ons/accelerants/ati (MIT licence; the Rage128 paths descend from the
 * X.org ATI driver, "Copyright 1999, 2000 ATI Technologies Inc., Precision Insight, VA Linux
 * Systems"):
 *
 *   rage128_draw.cpp  Rage128_FillRectangle     -> r128_fill()
 *   rage128_draw.cpp  Rage128_FillSpan          -> r128_fill_span()
 *   rage128_draw.cpp  Rage128_ScreenToScreenBlit-> r128_copy()
 *   rage128_draw.cpp  Rage128_InvertRectangle   -> r128_invert()
 *   rage128_draw.cpp  Rage128_EngineFlush       -> engine_flush()
 *   rage128_draw.cpp  Rage128_EngineInit        -> engine_init()
 *   rage128_init.cpp  WaitForFifo / WaitForIdle -> fifo_ready() / engine_idle()
 *   engine.cpp        engine_token {1, B_2D_ACCELERATION}, WaitEngineIdle
 *
 * The device list is the subset of the upstream `ati' driver's own kSupportedDevices whose chipset
 * enum selects the Rage128 engine -- the rows in drivers/gpu/ati/ati_ids.inc are those whose second
 * field is RAGE128_*, extracted from src/add-ons/kernel/drivers/graphics/ati/driver.cpp:74-151.
 * The 30 MACH64_* rows in the same table are deliberately not claimed here: their draw path is
 * mach64_draw.cpp, which is a different module file.  That separation is the point of shipping one
 * module per family instead of one blob.
 *
 * Four deliberate deviations from upstream, each forced by running in a kernel rather than a
 * userspace add-on.  They are listed here so a reader can audit them against the original:
 *
 * 1. No unbounded wait loops.  Upstream's WaitForFifo/WaitForIdle retry forever, calling
 *    Rage128_EngineReset() between attempts.  A kernel module must not be able to hang the machine,
 *    so both waits here are bounded by a tick deadline supplied by the kernel; when the deadline
 *    passes the module marks the engine dead, every later operation returns an error, and the
 *    kernel detaches it and finishes the work on the CPU.  The first failure is logged.
 * 2. No PLL access.  Rage128_EngineReset() forces R128_FORCE_GCP | R128_FORCE_PIPE3D_CP through
 *    the CLOCK_CNTL_INDEX/DATA pair before the soft reset.  Writing a card's clock registers is the
 *    operation that can blank a panel, so it is out of scope for a 2D module by SCos policy
 *    (see kernel/include/gpu_abi.h: SCOS_GPU_MODULE_TAKES_DISPLAY is refused).  What remains -- the
 *    pixel-cache flush and R128_SOFT_RESET_GUI -- is what the reset actually needs for the GUI
 *    engine, and the sequence order is upstream's.
 * 3. Destination clipping is set per operation instead of once at init.  Upstream programs the
 *    scissor to the whole 8191x8191 extent during EngineInit and lets the framebuffer's own bounds
 *    contain the damage.  Here each fill/copy sets the scissor to its destination rectangle, so an
 *    off-by-one in a size field cannot write one pixel into the start of the next scanline.
 * 4. Size fields use the hardware convention, not upstream's.  Rage128_ScreenToScreenBlit programs
 *    R128_DST_HEIGHT_WIDTH with (height + 1, width + 1).  The Rage 128 register semantics, and the
 *    X.org r128 driver's `R128Accel*BW' paths that Haiku's code descends from, treat those fields as
 *    counts-minus-one, so a blit of an N-pixel rectangle is programmed as N - 1.  This module writes
 *    N - 1.  Note that QEMU's model of this chip (hw/display/ati_2d.c) interprets the same fields as
 *    exact counts, so a GPU blit verified under QEMU covers one fewer pixel per axis than the
 *    rectangle that was requested; that is recorded here and in docs/migration/GPU-DRIVER-MODULES.md
 *    instead of being hidden by matching the emulator.  The fill path is programmed with exact
 *    counts, as upstream does.
 */

#include "gpu_abi.h"
#include "ati_regs.h"
/* A module may not import anything, so the few libc-ish helpers the compiler may emit calls for
 * live here.  They are kept simple on purpose: the packer refuses any undefined symbol, which is
 * what makes "a module can only reach the kernel through the export table" a structural fact. */
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
static u64 registers;                 /* the device's MMIO handle */
static u64 registers_size;
static u32 gui_master_cntl;           /* upstream's si.r128_dpGuiMasterCntl */
static struct scos_gpu_surface front, back;
static char logline[160];
static char describe[96];
static u32 blit_count, fill_count;
static int engine_dead, last_error;
/* Some boards - and the emulator this port is developed against - report GUI_ACTIVE as permanently
 * set: the bit stays high with nothing queued, so it cannot be used as an idle signal.  Waiting on it
 * would either have to be unbounded (never acceptable in a kernel module) or would detach an engine
 * that works.  So the status is sampled once, right after the GUI reset and before this driver has
 * submitted a command: if the card already claims to be busy the bounded wait is skipped and the
 * kernel's readback self-test decides, because it is the pixels that say whether the engine works.
 * On a card whose bit does drop nothing changes, and the wait runs as upstream wrote it. */
static int gui_busy_bit_unreliable;
static int refusal_logged;

static u32 in_reg(u32 offset)
{
    return X->read32(registers, offset);
}

static void out_reg(u32 offset, u32 value)
{
    X->write32(registers, offset, value);
}

static void out_reg_masked(u32 offset, u32 value, u32 mask)
{
    out_reg(offset, (in_reg(offset) & ~mask) | (value & mask));
}

static char *append_str(char *at, const char *text)
{
    while (*text) *at++ = *text++;
    return at;
}

static char *append_hex(char *at, u32 value)
{
    static const char digits[] = "0123456789abcdef";
    *at++ = '0';
    *at++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) *at++ = digits[(value >> shift) & 0xf];
    *at++ = ' ';
    return at;
}

/* Decimal, for counts that mean nothing in hex. */
static char *append_dec(char *at, u32 value)
{
    char digits[10];
    int i = 0;
    do { digits[i++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (i) *at++ = digits[--i];
    return at;
}

static void note(const char *text)
{
    if (X->log) X->log(text);
}

/* ---------------------------------------------------------------- waits ---- */

/* Upstream: `while (true) { for (i<R128_TIMEOUT) if (slots>=entries) return; reset engine; }'.
 * Here: one bounded pass, and a dead-engine latch afterwards. */
static int fifo_ready(u32 entries)
{
    u64 deadline = X->ticks() + 2000000u;   /* ~1 ms of cycles at 2 GHz, then give up */
    for (;;) {
        if ((in_reg(R128_GUI_STAT) & R128_GUI_FIFOCNT_MASK) >= entries) return 1;
        if (engine_dead) return 0;
        if (X->ticks() > deadline) break;
        X->spin(64);
    }
    engine_dead = 1;
    last_error = -1;
    logline[0] = 'a'; logline[1] = 't'; logline[2] = 'i'; logline[3] = ':';
    logline[4] = ' '; logline[5] = 'F'; logline[6] = 'I'; logline[7] = 'F'; logline[8] = 'O';
    logline[9] = ' '; logline[10] = 's'; logline[11] = 't'; logline[12] = 'a'; logline[13] = 'l';
    logline[14] = 'l'; logline[15] = 'e'; logline[16] = 'd';
    logline[17] = ' ';
    {
        char *at = append_hex(logline + 18, in_reg(R128_GUI_STAT));
        at = append_str(at, " GUI_STAT: engine detached, CPU path resumes");
        *at = 0;
    }
    note(logline);
    return 0;
}

/* Upstream's Rage128_EngineFlush(): dirty pixel cache back into memory, then poll PC_BUSY. */
static void engine_flush(void)
{
    out_reg_masked(R128_PC_NGUI_CTLSTAT, R128_PC_FLUSH_ALL, R128_PC_FLUSH_ALL);
    for (u32 i = 0; i < 100000u; i++)
        if (!(in_reg(R128_PC_NGUI_CTLSTAT) & R128_PC_BUSY)) return;
}

/* The GUI half of upstream's Rage128_EngineReset(), without its PLL writes (deviation 2). */
static void engine_reset(void)
{
    u32 control = in_reg(R128_GEN_RESET_CNTL);
    engine_flush();
    out_reg(R128_GEN_RESET_CNTL, control | R128_SOFT_RESET_GUI);
    in_reg(R128_GEN_RESET_CNTL);
    out_reg(R128_GEN_RESET_CNTL, control & ~R128_SOFT_RESET_GUI);
    in_reg(R128_GEN_RESET_CNTL);
}

/* Upstream's WaitForIdle(): drain the FIFO, then wait for GUI_ACTIVE to drop and flush. */
static s32 engine_idle(void)
{
    if (!fifo_ready(64)) return -1;
    if (gui_busy_bit_unreliable) { engine_flush(); return 0; }
    u64 deadline = X->ticks() + 4000000u;
    for (;;) {
        if (!(in_reg(R128_GUI_STAT) & R128_GUI_ACTIVE)) {
            engine_flush();
            return 0;
        }
        if (engine_dead) return -1;
        if (X->ticks() > deadline) {
            engine_dead = 1;
            last_error = -2;
            note("ati: engine did not go idle before the deadline; engine detached");
            return -1;
        }
        X->spin(64);
    }
}

/* ---------------------------------------------------------- surfaces ---- */

/* The pitch register for the Rage128 data path counts in units of eight pixels for a given pixel
 * size, which is how upstream programs R128_DEFAULT_PITCH as `h_display / 8'. */
static u32 pitch_register(const struct scos_gpu_surface *surface)
{
    u32 pixels = surface->pitch_bytes / (surface->bytes_per_pixel ? surface->bytes_per_pixel : 4);
    u32 value = pixels / 8;
    return value > 0x3fffu ? 0x3fffu : (value ? value : 1);
}

static const struct scos_gpu_surface *surface_of(u32 which)
{
    if (which == SCOS_GPU_SURFACE_BACK && back.pitch_bytes) return &back;
    return &front;
}

/* Deviation 3: scissor to the destination rectangle before every operation. */
static void clip_to(s32 x, s32 y, s32 w, s32 h)
{
    u32 left = (u32)x, top = (u32)y;
    u32 right = (u32)(x + w - 1), bottom = (u32)(y + h - 1);
    out_reg(R128_SC_TOP_LEFT, (top << 16) | left);
    out_reg(R128_SC_BOTTOM_RIGHT, (bottom << 16) | right);
}

static int rect_ok(const struct scos_gpu_surface *surface, s32 x, s32 y, s32 w, s32 h)
{
    if (w <= 0 || h <= 0) return 0;
    if (x < 0 || y < 0) return 0;
    if ((u32)x + (u32)w > surface->width || (u32)y + (u32)h > surface->height) return 0;
    /* Everything must stay inside the aperture: the hardware address is offset + y*pitch. */
    u64 end = (u64)surface->offset_bytes + (u64)(y + h) * surface->pitch_bytes;
    if (end <= X->device->vram_bytes) return 1;
    /* Say so once, with the numbers: a compositor that silently loses every accelerated rectangle
     * is the kind of bug that takes a week to notice, and the log is the only view of this driver. */
    if (!refusal_logged) {
        refusal_logged = 1;
        char *at = append_str(logline, "ati: rectangle needs ");
        at = append_dec(at, (u32)end);
        at = append_str(at, " B, device has ");
        at = append_dec(at, (u32)X->device->vram_bytes);
        *at = 0;
        note(logline);
    }
    return 0;
}

/* ------------------------------------------------------------ drawing ---- */

/* Ported from Rage128_FillRectangle(): solid-colour pattern copy, exact counts in
 * R128_DST_WIDTH_HEIGHT (width << 16 | height). */
static s32 r128_fill(void *context, u32 which, s32 x, s32 y, s32 width, s32 height, u32 color)
{
    (void)context;
    const struct scos_gpu_surface *surface = surface_of(which);
    if (engine_dead) return -1;
    if (!rect_ok(surface, x, y, width, height)) return -1;
    if (!fifo_ready(8)) return -1;
    out_reg(R128_DST_OFFSET, surface->offset_bytes);
    out_reg(R128_DST_PITCH, pitch_register(surface));
    out_reg(R128_DP_BRUSH_FRGD_CLR, color);
    out_reg(R128_DP_WRITE_MASK, 0xffffffffu);
    out_reg(R128_DP_CNTL, R128_DST_X_LEFT_TO_RIGHT | R128_DST_Y_TOP_TO_BOTTOM);
    clip_to(x, y, width, height);
    out_reg(R128_DP_GUI_MASTER_CNTL,
            gui_master_cntl | R128_GMC_BRUSH_SOLID_COLOR | R128_GMC_SRC_DATATYPE_COLOR |
            R128_ROP3_P | R128_GMC_DST_PITCH_OFFSET_CNTL);
    out_reg(R128_DST_Y_X, ((u32)y << 16) | (u32)x);
    out_reg(R128_DST_WIDTH_HEIGHT, ((u32)width << 16) | (u32)height);
    fill_count++;
    if (!fifo_ready(2)) return -1;
    out_reg(R128_SC_TOP_LEFT, 0);
    out_reg(R128_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    return 0;
}

/* Ported from Rage128_ScreenToScreenBlit(), including its overlap direction rule, with the
 * count-minus-one size convention of deviation 4. */
static s32 r128_copy(void *context, u32 dst_which, s32 dst_x, s32 dst_y, u32 src_which,
                     s32 src_x, s32 src_y, s32 width, s32 height)
{
    (void)context;
    const struct scos_gpu_surface *dst = surface_of(dst_which);
    const struct scos_gpu_surface *src = surface_of(src_which);
    if (engine_dead) return -1;
    if (!rect_ok(dst, dst_x, dst_y, width, height) || !rect_ok(src, src_x, src_y, width, height))
        return -1;
    u32 command = 0;
    s32 sx = src_x, sy = src_y, dx = dst_x, dy = dst_y;
    int same = dst == src;
    if (!same || dst_x <= src_x) command |= R128_DST_X_LEFT_TO_RIGHT;
    else { sx += width; dx += width; }
    if (!same || dst_y <= src_y) command |= R128_DST_Y_TOP_TO_BOTTOM;
    else { sy += height; dy += height; }
    if (!fifo_ready(10)) return -1;
    out_reg(R128_DP_CNTL, command);
    out_reg(R128_DST_OFFSET, dst->offset_bytes);
    out_reg(R128_DST_PITCH, pitch_register(dst));
    out_reg(R128_SRC_OFFSET, src->offset_bytes);
    out_reg(R128_SRC_PITCH, pitch_register(src));
    out_reg(R128_DP_SRC_FRGD_CLR, 0xffffffffu);
    out_reg(R128_DP_SRC_BKGD_CLR, 0u);
    out_reg(R128_DP_WRITE_MASK, 0xffffffffu);
    clip_to(dst_x, dst_y, width, height);
    out_reg(R128_DP_GUI_MASTER_CNTL,
            gui_master_cntl | R128_GMC_BRUSH_NONE | R128_GMC_SRC_DATATYPE_COLOR |
            R128_GMC_DP_SRC_SOURCE_MEMORY | R128_ROP3_S |
            R128_GMC_SRC_PITCH_OFFSET_CNTL | R128_GMC_DST_PITCH_OFFSET_CNTL);
    out_reg(R128_SRC_Y_X, ((u32)sy << 16) | (u32)sx);
    out_reg(R128_DST_Y_X, ((u32)dy << 16) | (u32)dx);
    out_reg(R128_DST_HEIGHT_WIDTH, (((u32)height - 1u) << 16) | ((u32)width - 1u));
    blit_count++;
    if (!fifo_ready(2)) return -1;
    out_reg(R128_SC_TOP_LEFT, 0);
    out_reg(R128_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    return 0;
}

/* Ported from Rage128_FillSpan(): runs are (x_start, x_end) pairs, inclusive, and a run of
 * non-positive width is dropped rather than clamped, exactly as upstream does. */
static s32 r128_fill_span(void *context, s32 y, const s32 *runs, u32 run_count, u32 color)
{
    (void)context;
    if (engine_dead) return -1;
    const struct scos_gpu_surface *surface = &front;
    if (y < 0 || (u32)y >= surface->height) return -1;
    if (!fifo_ready(4)) return -1;
    out_reg(R128_DST_OFFSET, surface->offset_bytes);
    out_reg(R128_DST_PITCH, pitch_register(surface));
    out_reg(R128_DP_BRUSH_FRGD_CLR, color);
    out_reg(R128_DP_CNTL, R128_DST_X_LEFT_TO_RIGHT | R128_DST_Y_TOP_TO_BOTTOM);
    out_reg(R128_DP_GUI_MASTER_CNTL,
            gui_master_cntl | R128_GMC_BRUSH_SOLID_COLOR | R128_GMC_SRC_DATATYPE_COLOR |
            R128_ROP3_P | R128_GMC_DST_PITCH_OFFSET_CNTL);
    for (u32 i = 0; i < run_count; i++) {
        s32 left = runs[i * 2], right = runs[i * 2 + 1];
        s32 width = right - left + 1;
        if (width <= 0) continue;
        if (left < 0) { width += left; left = 0; }
        if ((u32)left + (u32)width > surface->width) width = (s32)(surface->width - (u32)left);
        if (width <= 0) continue;
        if (!fifo_ready(2)) return -1;
        out_reg(R128_SC_TOP_LEFT, ((u32)y << 16) | (u32)left);
        out_reg(R128_SC_BOTTOM_RIGHT, ((u32)y << 16) | (u32)(left + width - 1));
        out_reg(R128_DST_Y_X, ((u32)y << 16) | (u32)left);
        out_reg(R128_DST_WIDTH_HEIGHT, ((u32)width << 16) | 1u);
    }
    if (!fifo_ready(2)) return -1;
    out_reg(R128_SC_TOP_LEFT, 0);
    out_reg(R128_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    return 0;
}

/* Ported from Rage128_InvertRectangle(): the read-modify-write ROP, kept because it is the one
 * operation in this engine that needs the source and destination in the same surface. */
static s32 r128_invert(void *context, u32 which, s32 x, s32 y, s32 width, s32 height)
{
    (void)context;
    const struct scos_gpu_surface *surface = surface_of(which);
    if (engine_dead) return -1;
    if (!rect_ok(surface, x, y, width, height)) return -1;
    if (!fifo_ready(8)) return -1;
    out_reg(R128_DST_OFFSET, surface->offset_bytes);
    out_reg(R128_SRC_OFFSET, surface->offset_bytes);
    out_reg(R128_DST_PITCH, pitch_register(surface));
    out_reg(R128_SRC_PITCH, pitch_register(surface));
    out_reg(R128_DP_CNTL, R128_DST_X_LEFT_TO_RIGHT | R128_DST_Y_TOP_TO_BOTTOM);
    clip_to(x, y, width, height);
    out_reg(R128_DP_GUI_MASTER_CNTL,
            gui_master_cntl | R128_GMC_BRUSH_NONE | R128_GMC_SRC_DATATYPE_COLOR |
            R128_GMC_DP_SRC_SOURCE_MEMORY | R128_ROP3_Dn |
            R128_GMC_SRC_PITCH_OFFSET_CNTL | R128_GMC_DST_PITCH_OFFSET_CNTL);
    out_reg(R128_DST_Y_X, ((u32)y << 16) | (u32)x);
    out_reg(R128_DST_WIDTH_HEIGHT, ((u32)width << 16) | (u32)height);
    if (!fifo_ready(2)) return -1;
    out_reg(R128_SC_TOP_LEFT, 0);
    out_reg(R128_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    return 0;
}

static s32 r128_wait_idle(void *context)
{
    (void)context;
    return engine_idle();
}

/* ------------------------------------------------------------- bring-up ---- */

/* Ported from Rage128_EngineInit(), for the one pixel format SCos scans out today. */
static s32 engine_init(void)
{
    u32 data_type;
    switch (front.bytes_per_pixel) {
    case 1: data_type = 2; break;
    case 2: data_type = 4; break;
    case 4: data_type = 6; break;
    default:
        note("ati: refusing a colour depth SCos has no upstream encoding for");
        return -1;
    }
    out_reg(R128_SCALE_3D_CNTL, 0);
    engine_reset();
    {
        u32 stat = in_reg(R128_GUI_STAT);
        if (stat & R128_GUI_ACTIVE) {
            gui_busy_bit_unreliable = 1;
            char *at = append_str(logline, "ati: GUI_ACTIVE high at bring-up (");
            at = append_hex(at, stat);
            at = append_str(at, "); idle decided by readback");
            *at = 0;
            note(logline);
        }
    }
    gui_master_cntl = (data_type << R128_GMC_DST_DATATYPE_SHIFT) | R128_GMC_CLR_CMP_CNTL_DIS |
                      R128_GMC_AUX_CLIP_DIS;
    if (!fifo_ready(2)) return -1;
    /* The defaults describe the visible surface; an operation that needs a different pitch or base
     * sets the corresponding GMC_*_PITCH_OFFSET_CNTL bit and programs its own registers, as
     * upstream does for off-screen surfaces. */
    out_reg(R128_DEFAULT_OFFSET, front.offset_bytes);
    out_reg(R128_DEFAULT_PITCH, pitch_register(&front));
    out_reg(R128_AUX_SC_CNTL, 0);
    out_reg(R128_DEFAULT_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    out_reg(R128_SC_TOP_LEFT, 0);
    out_reg(R128_SC_BOTTOM_RIGHT, R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX);
    if (!fifo_ready(8)) return -1;
    out_reg(R128_DST_BRES_ERR, 0);
    out_reg(R128_DST_BRES_INC, 0);
    out_reg(R128_DST_BRES_DEC, 0);
    out_reg(R128_DP_BRUSH_FRGD_CLR, 0xffffffffu);
    out_reg(R128_DP_BRUSH_BKGD_CLR, 0u);   /* 0x1478, from Haiku rage128.h:91 and QEMU ati_regs.h */
    out_reg(R128_DP_SRC_FRGD_CLR, 0xffffffffu);
    out_reg(R128_DP_SRC_BKGD_CLR, 0u);
    out_reg(R128_DP_WRITE_MASK, 0xffffffffu);
    out_reg(R128_DP_GUI_MASTER_CNTL, gui_master_cntl | R128_GMC_BRUSH_SOLID_COLOR |
            R128_GMC_SRC_DATATYPE_COLOR);
    out_reg_masked(R128_DP_DATATYPE, 0, R128_HOST_BIG_ENDIAN_EN);
    return engine_idle();
}

static s32 r128_set_surfaces(void *context, const struct scos_gpu_surface *new_front,
                              const struct scos_gpu_surface *new_back)
{
    (void)context;
    if (!new_front || !new_front->width || !new_front->height || !new_front->pitch_bytes)
        return -1;
    front = *new_front;
    if (new_back && new_back->pitch_bytes) back = *new_back;
    else { back.offset_bytes = 0; back.pitch_bytes = 0; back.width = 0; back.height = 0;
           back.bytes_per_pixel = front.bytes_per_pixel; }
    return engine_init();
}

/* The back buffer lives in this card's own memory, above the visible surface, so a flip is a
 * VRAM-to-VRAM bitblt instead of a CPU copy across the bus.  Decline rather than squeeze: if the
 * aperture cannot hold a second full surface, the kernel keeps its RAM back buffer and only the
 * fills move to the GPU. */
static s32 r128_vram_window(void *context, u32 *offset_out, u32 *bytes_out)
{
    (void)context;
    u32 vram = X->device->vram_bytes;
    if (!vram) return -1;
    u32 needed = front.pitch_bytes * front.height;
    u32 start = (needed + 65535u) & ~65535u;     /* keep the surface 64K aligned, like upstream's
                                                   * cursor/overlay offsets */
    if (start + needed > vram) return -1;
    if (needed > 0x1000000u) return -1;
    *offset_out = start;
    *bytes_out = needed;
    return 0;
}

static const char *r128_describe(void *context)
{
    (void)context;
    const char *fixed = "Rage 128 GUI engine (fill, bitblt, spans, invert); Haiku ati/rage128_draw";
    char *at = describe;
    const char *from = fixed;
    while (*from) *at++ = *from++;
    const char *stats = " | ops: ";
    while (*stats) *at++ = *stats++;
    char digits[12];
    u32 value = fill_count;
    int i = 0;
    do { digits[i++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (i) *at++ = digits[--i];
    const char *mid = " fill, ";
    while (*mid) *at++ = *mid++;
    value = blit_count; i = 0;
    do { digits[i++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (i) *at++ = digits[--i];
    const char *tail = " blit";
    while (*tail) *at++ = *tail++;
    if (engine_dead) { const char *bad = ", detached"; while (*bad) *at++ = *bad++; }
    *at = 0;
    return describe;
}

static void r128_teardown(void *context)
{
    (void)context;
    if (engine_dead) return;
    engine_idle();
    /* Leave the engine in the idle, unclipped state it was found in; no clock or mode writes. */
    out_reg(R128_DP_GUI_MASTER_CNTL, gui_master_cntl | R128_GMC_BRUSH_SOLID_COLOR |
            R128_GMC_SRC_DATATYPE_COLOR);
}

/* Field order matches struct scos_gpu_engine_ops in kernel/include/gpu_abi.h; the last two
 * entries are the optional tier, so a kernel built against the base set still binds this module. */
static struct scos_gpu_engine_ops ops = {
    sizeof(ops), SCOS_GPU_ABI_VERSION, 0,
    r128_fill, r128_copy, r128_wait_idle,
    r128_vram_window, r128_set_surfaces, r128_describe, r128_teardown,
    r128_fill_span, r128_invert
};

/* The id rules this module claims: same struct, same meaning as the kernel's generated table, so
 * the loader can require every entry to appear in the family record it matched. */
const struct scos_gpu_pci_id scos_module_ids[] = {
#include "ati_ids.inc"
};

int scos_module_init(const struct scos_gpu_exports *exports, struct scos_gpu_engine_ops **out_ops)
{
    X = exports;
    if (!X || X->abi != SCOS_GPU_ABI_VERSION || X->size < sizeof(*X)) return -1;
    if (!out_ops) return -1;
    const struct scos_gpu_device_info *device = X->device;
    if (!device || device->size < sizeof(*device)) return -1;
    if (device->vendor != 0x1002) return -1;
    int claimed = 0;
    for (unsigned i = 0; i < sizeof(scos_module_ids) / sizeof(scos_module_ids[0]); i++)
        if (scos_module_ids[i].device == device->device) claimed = 1;
    if (!claimed) {
        note("ati: this module drives only the Rage128 rows of the upstream table; device left on "
             "the CPU path");
        return -1;
    }
    /* Registers: a memory BAR that is not the aperture holding the scanout surface.  QEMU's
     * ati-vga is BAR0 aperture / BAR2 registers, which this rule finds without any chip-specific
     * table, and the choice is logged so a boot log shows what was mapped. */
    u64 surface_bar = 0, register_bar = 0, register_bytes = 0;
    for (int i = 0; i < 6; i++) {
        u64 base = device->bar_base[i], bytes = device->bar_bytes[i];
        if (!base || !bytes || device->bar_is_io[i]) continue;
        if (base == device->framebuffer_base) { surface_bar = base; continue; }
        if (device->framebuffer_base >= base &&
            device->framebuffer_base < base + bytes) { surface_bar = base; continue; }
        if (bytes <= 0x100000ull && (bytes & (bytes - 1)) == 0 && bytes >= 0x1000 &&
            !register_bar) { register_bar = base; register_bytes = bytes; }
    }
    if (!register_bar) { note("ati: no register BAR found on this function; staying on CPU"); return -1; }
    if (!surface_bar) surface_bar = register_bar;   /* aperture and regs may share a BAR (Mach64) */
    u64 mapped = 0;
    /* The register block of a Rage128 runs past 0x1800 (GUI_STAT is 0x1740), so mapping a page of it
     * would leave the status and FIFO registers unreachable and every access in this driver would be
     * dropped by the kernel's bounds check.  Ask for the whole BAR; the kernel maps what it can and
     * reports the real size back, which is what `registers_size' then means. */
    registers = X->map(register_bar, register_bytes, 0, &mapped);
    if (!registers) { note("ati: MMIO mapping failed; staying on CPU"); return -1; }
    registers_size = mapped ? mapped : register_bytes;
    if (registers_size < R128_GUI_STAT + 4u) {
        note("ati: register BAR is smaller than the Rage128 block; staying on CPU");
        return -1;
    }
    front.width = device->width; front.height = device->height;
    front.pitch_bytes = device->pitch; front.bytes_per_pixel = device->bytes_per_pixel;
    /* Where inside the aperture the visible surface starts: upstream's si.frameBufferOffset. */
    front.offset_bytes = device->framebuffer_base >= surface_bar
        ? (u32)(device->framebuffer_base - surface_bar) : 0u;
    if (engine_init() != 0) {
        note("ati: engine bring-up timed out; the card is left to the CPU compositor");
        return -1;
    }
    ops.context = 0;
    /* Stamp the table itself rather than trusting the bytes that were packed: the ABI and the size are
     * what the kernel validates before it calls anything, and a module that reports them from its own
     * running code cannot be misread. */
    ops.size = (u32)sizeof(ops);
    ops.abi = SCOS_GPU_ABI_VERSION;
    *out_ops = &ops;
    note("ati: Rage128 GUI engine ready");
    return 0;
}

void scos_module_teardown(struct scos_gpu_engine_ops *bound)
{
    if (bound) r128_teardown(bound->context);
}
