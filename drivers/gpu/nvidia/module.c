/*
 * SCos GPU display module - NVIDIA Blackwell (GB20x) identification driver.
 *
 * What this module is, stated first so nothing below has to be inferred: it reads one register out of
 * the chip's own power-management block and reports what the silicon says it is.  It does not program
 * the display, does not start an engine, does not touch the frame buffer, and writes no register at all.
 * Binding it therefore cannot break a desktop: every drawing path in the kernel checks the operation
 * pointer first, and this module's is null, so the CPU compositor keeps painting exactly as it did
 * before the module loaded.
 *
 * Why it exists at all: "a family was named from a table" and "a driver read this chip" are different
 * claims.  Before this module, a Blackwell display function was recognised from its PCI id and then
 * reported as a family with no driver, so the boot log could say which product name the id maps to but
 * had read nothing from the card.  After it, SCos has looked at NV_PMC_BOOT_0 on the device and can say
 * what the chip answered, which BAR the register block lives in, and whether this function is the one
 * feeding the console - and if the register block does not answer, it says *that*, which is a fact about
 * the machine rather than a guess about it.
 *
 * What remains before drawing is possible here, in plain terms: a Blackwell display engine is not
 * configured by a guest driver the way a 1994 CL-GD5446 is.  Channel creation and the copy class are
 * documented well enough to write against (the FIFO host class and the DMA-copy classes are published:
 * clc*6f and clc*b5), but the chip's display and power block is brought up through its system
 * processor, and the image that does that booting is not redistributable.  This module is the step
 * before that, not a substitute for it.
 *
 * Provenance of every register offset and field used here is in blackwell_regs.h, and points at
 * NVIDIA's own published, MIT-licensed register documentation.  No driver source from any operating
 * system was read to write this file.
 */
#include "gpu_abi.h"
#include "blackwell_regs.h"

/* A module may not import anything, so it supplies the one memory helper it needs. */
void *memset(void *pointer, int value, unsigned long bytes)
{
    unsigned char *at = pointer;
    for (unsigned long i = 0; i < bytes; i++) at[i] = (unsigned char)value;
    return pointer;
}

static const struct scos_gpu_exports *X;

struct inventory {
    u64 physical, mapped_bytes;         /* where the block is, as the PCI function reports it */
    u64 handle;                         /* what the kernel's accessors want */
    u32 boot0, boot0_repeat;
    u32 reads;                          /* register reads issued: two, of one offset, both reported */
    u32 writes;                         /* always 0; see reg_read() below, and there is no reg_write() */
    int answered;                       /* the block returned something other than 0 and other than -1 */
    int owns_console;                   /* the firmware's scanout surface sits in one of these BARs */
    u64 console_offset;                 /* and this far into it */
    /* Built once at the end of init.  160 is the kernel's own limit for a description, and it is
     * asserted by the host harness rather than trusted here: a clipped tail would drop the clause
     * that says whose screen this is, which is the part a reader came for. */
    char text[176];
};

static struct inventory inv;

static void note(const char *text)
{
    if (X && X->log) X->log(text);
}

/* One accessor, and deliberately no write accessor: "this driver never writes the chip" is a property of
 * the code, not of the comment above.  A bound engine module is *allowed* to write - the kernel gives it
 * write32 because a blitter has to be configured - and an engine driver needs that.  This one does not,
 * so it does not take it, and the read count below is what the report prints. */
static u32 reg_read(u32 offset)
{
    inv.reads++;
    return X->read32(inv.handle, offset);
}

/* ------------------------------------------------------------------ formatting ----
 * No printf in a module: the kernel refuses to pass a format string across the boundary, so a module
 * formats into its own buffer.  These four helpers are the whole formatting machinery. */
struct buf {
    char *at, *end;                     /* `end' is one past the last byte that may be written */
};

static void put_char(struct buf *b, char c)
{
    if (b->at < b->end) *b->at++ = c;
}

static void put_text(struct buf *b, const char *s)
{
    while (*s) put_char(b, *s++);
}

static void put_hex(struct buf *b, u32 value, int digits)
{
    static const char nibble[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) put_char(b, nibble[(value >> (i * 4)) & 0xf]);
}

static void put_dec(struct buf *b, u32 value)
{
    char digits[10];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) put_char(b, digits[--n]);
}

/* The architecture numbers NVIDIA's newest published field table names.  Anything else is printed as a
 * number and labelled as unnamed, because the alternative - guessing that a Blackwell chip reuses an
 * older generation's label - would put a wrong fact in front of a user. */
static const char *architecture_name(u32 architecture)
{
    switch (architecture) {
    case NV_PMC_BOOT_0_ARCHITECTURE_GV100: return "Volta";
    case NV_PMC_BOOT_0_ARCHITECTURE_TU100: return "Turing";
    case NV_PMC_BOOT_0_ARCHITECTURE_GA100: return "Ampere";
    default: return 0;
    }
}

static void build_text(void)
{
    struct buf b = { inv.text, inv.text + sizeof(inv.text) - 1 };
    u32 architecture = NV_PMC_BOOT_0_ARCHITECTURE(inv.boot0);
    const char *name = architecture_name(architecture);
    static const char major[] = "0123456789ABCDEF";
    memset(inv.text, 0, sizeof(inv.text));
    put_text(&b, "NV_PMC_BOOT_0=0x");
    put_hex(&b, inv.boot0, 8);
    put_text(&b, " at BAR 0x");
    put_hex(&b, (u32)inv.physical, 8);
    if (inv.physical >> 32) {
        put_text(&b, " (high 0x");
        put_hex(&b, (u32)(inv.physical >> 32), 8);
        put_text(&b, ")");
    }
    put_text(&b, " arch 0x");
    put_hex(&b, architecture, 2);
    /* Short on purpose: the kernel keeps 160 characters, and a sentence clipped at the end would hide
     * the one clause a user most needs - whose screen this is. */
    put_char(&b, ' ');
    put_text(&b, name ? name : "(arch not in the published table)");
    put_text(&b, ", impl 0x");
    put_hex(&b, NV_PMC_BOOT_0_IMPLEMENTATION(inv.boot0), 1);
    put_text(&b, " rev ");
    put_char(&b, major[NV_PMC_BOOT_0_MAJOR_REVISION(inv.boot0)]);
    put_char(&b, '.');
    put_dec(&b, NV_PMC_BOOT_0_MINOR_REVISION(inv.boot0));
    put_text(&b, "; reads ");
    put_dec(&b, inv.reads);
    put_text(&b, ", writes ");
    put_dec(&b, inv.writes);
    put_text(&b, inv.owns_console ? " feeds the console" : " not the console owner");
    *b.at = 0;
}

/* ------------------------------------------------------------------ engine ops ----
 * Every operation a bound engine may implement, and this module implements none of them but the
 * description.  A caller that asks this card to fill a rectangle is told no and falls back to the CPU
 * path, unchanged: the kernel's predicates all check the pointer before the call. */
static const char *describe(void *context)
{
    (void)context;
    return inv.text;
}

static struct scos_gpu_engine_ops ops = {
    sizeof(ops), SCOS_GPU_ABI_VERSION, 0,
    0, 0, 0,                            /* fill, copy, wait_idle: none offered */
    0, 0,                               /* no VRAM window, no surface handover */
    describe, 0,                        /* description only, nothing to unwind */
    0, 0,                               /* no span list, no invert */
};

/* The functions this module reads, and no others.  The loader cross-checks every row against the family
 * table the kernel detected from, so a module cannot claim a chip nobody found.  These are the Blackwell
 * display ids: the family record also covers older chips, and for those this driver refuses in init
 * rather than assuming a Blackwell register map still applies. */
const struct scos_gpu_pci_id scos_module_ids[] = {
#include "nvidia_ids.inc"
};

/* Nothing to restore, which is the point: a driver that only read a register leaves the chip exactly as
 * it found it, and that is why this module is safe to bind on a machine whose display the firmware is
 * still driving.  The mapping belongs to the kernel; it costs page tables and no device state. */
void scos_module_teardown(struct scos_gpu_engine_ops *bound)
{
    (void)bound;
    inv.handle = 0;
    inv.physical = 0;
    inv.answered = 0;
}

/* The register aperture is the function's own, not the frame buffer: on these chips the control block is
 * a BAR of its own and the surface the firmware left behind sits behind another.  Picking by index
 * would be a guess about a layout nobody here can measure, so the rule is size-and-place based: the
 * first memory BAR that is not the aperture, is not the aperture's parent, and is large enough to hold
 * the block - and the chip's answer is what validates the choice, because a wrong BAR reads back
 * nothing, which is refused below rather than reported. */
static int find_register_bar(const struct scos_gpu_device_info *device, u64 *base, u64 *bytes)
{
    for (int i = 0; i < 6; i++) {
        u64 bar_base = device->bar_base[i], bar_bytes = device->bar_bytes[i];
        if (!bar_base || !bar_bytes || device->bar_is_io[i]) continue;
        if (bar_bytes < 4096u) continue;
        if (device->framebuffer_base >= bar_base &&
            device->framebuffer_base < bar_base + bar_bytes) continue;
        *base = bar_base;
        *bytes = bar_bytes;
        return 1;
    }
    return 0;
}

int scos_module_init(const struct scos_gpu_exports *exports, struct scos_gpu_engine_ops **out_ops)
{
    /* Start clean.  Nothing here needs the previous run's answer, and carrying one over would let a
     * second bind report an inventory the current chip never gave: `reads', `writes' and
     * `owns_console' are all measurements of *this* attempt. */
    inv.boot0 = inv.boot0_repeat = 0;
    inv.reads = inv.writes = 0;
    inv.answered = inv.owns_console = 0;
    inv.console_offset = 0;
    inv.handle = inv.physical = inv.mapped_bytes = 0;
    inv.text[0] = 0;
    X = exports;
    if (!X || X->abi != SCOS_GPU_ABI_VERSION || X->size < sizeof(*X)) return -1;
    if (!out_ops) return -1;
    const struct scos_gpu_device_info *device = X->device;
    if (!device || device->size < sizeof(*device)) return -1;
    if (device->vendor != 0x10de) {
        note("nvidia: loaded for a function whose vendor is not NVIDIA; refusing to read it");
        return -1;
    }
    if (!find_register_bar(device, &inv.physical, &inv.mapped_bytes)) {
        note("nvidia: no BAR large enough to hold the power-management block on this function");
        return -1;
    }
    u64 mapped = 0;
    u64 handle = X->map(inv.physical, inv.mapped_bytes, 0, &mapped);
    if (!handle) {
        note("nvidia: mapping the register BAR failed; the chip stays unread");
        inv.physical = 0;
        return -1;
    }
    if ((mapped ? mapped : inv.mapped_bytes) < NV_PMC_BOOT_0 + 4u) {
        note("nvidia: the mapped register window is smaller than the boot register");
        inv.physical = 0;
        return -1;
    }
    inv.handle = handle;

    inv.boot0 = reg_read(NV_PMC_BOOT_0);
    inv.boot0_repeat = reg_read(NV_PMC_BOOT_0);
    if (inv.boot0 == 0xffffffffu || inv.boot0 == 0u) {
        /* Either the function has its memory decoding disabled, or the BAR the firmware reported is
         * not where the control block lives.  Both mean "no inventory", and inventing one is exactly
         * the failure this check exists to prevent. */
        note("nvidia: the register block answers nothing readable; reporting no inventory");
        inv.answered = 0;
        return -1;
    }
    if (inv.boot0 != inv.boot0_repeat) {
        note("nvidia: the boot register changed between two reads; not trusting it");
        inv.answered = 0;
        return -1;
    }
    inv.answered = 1;

    for (int i = 0; i < 6; i++) {
        u64 bar_base = device->bar_base[i], bar_bytes = device->bar_bytes[i];
        if (!bar_base || !bar_bytes || device->bar_is_io[i]) continue;
        if (device->framebuffer_base >= bar_base &&
            device->framebuffer_base < bar_base + bar_bytes) {
            inv.owns_console = 1;
            inv.console_offset = device->framebuffer_base - bar_base;
        }
    }

    build_text();
    char line[192];
    struct buf b = { line, line + sizeof(line) - 1 };
    put_text(&b, "nvidia: ");
    put_hex(&b, device->vendor, 4);
    put_char(&b, ':');
    put_hex(&b, device->device, 4);
    put_text(&b, " at ");
    put_dec(&b, device->bus);
    put_char(&b, ':');
    put_dec(&b, device->slot);
    put_char(&b, '.');
    put_dec(&b, device->function);
    put_text(&b, " identified from the chip's own boot register: ");
    put_text(&b, inv.text);
    *b.at = 0;
    note(line);

    *out_ops = &ops;
    return 0;
}
