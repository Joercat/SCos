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
    u32 usermode_class;                 /* NV_USERMODE_CFG0's class number, read from the device */
    u64 mapping_bytes;                  /* what the kernel actually mapped, which its cap may shrink */
    u64 window_handle;                  /* the window's own mapping, when the register one stops short */
    int window_direct;                  /* reached through the register mapping: no second mapping taken */
    int window_unreachable;             /* neither mapping reaches the documented page: says so, not "silent" */
    u64 timer_start, timer_end;         /* PTIMER as that window exposes it, in nanoseconds */
    int window_live;                    /* the class register answered with something other than 0 or -1 */
    int timer_ticking;                  /* and the clock moved between the two samples */
    u32 reads;                          /* register reads issued, counted as they are made and printed verbatim */
    u32 writes;                         /* always 0; see reg_read() below, and there is no reg_write() */
    int answered;                       /* the block returned something other than 0 and other than -1 */
    int owns_console;                   /* the firmware's scanout surface sits in one of these BARs */
    u64 console_offset;                 /* and this far into it */
    /* The engine inventory, as the chip's own device-info table described it.  These fields exist because
     * a submission has to name an engine and a runlist, and the only honest source for either on a
     * generation whose register manual was never published is the silicon.  `top_layout' is the part a
     * reader needs: it separates "this chip has no copy engine" from "this driver could not find the list".
     */
    u32 top_layout;                     /* 0 no table decoded, 1 the 0x22700 array, 2 the 0x22800 array */
    u32 top_devices, top_decoded;       /* devices named, entries accepted, by the table that won */
    u32 top_lce, top_graphics, top_vic, top_copy, top_enc, top_dec, top_sec, top_gsp, top_jpg, top_other;
    u32 top_ce_pri, top_ce_inst, top_ce_runlist, top_ce_engine;
    int top_ce_runlist_valid, top_ce_engine_valid;   /* the entry's own valid bits: 0 is a value, "?" is not */
    int top_ce_found;                   /* an LCE device also carried a PRI base: a place to write next */
    u32 top_cfg, top_cfg_devices, top_cfg_rows_per_device, top_cfg_rows;
    u32 top_cfg1;                     /* the second block's CFG, 0x324fc: asked, and reported if unanswered */
    u32 top_rows;                     /* rows actually read, so an inventory says how much was asked */
    u32 top_v2_cfg;                   /* the CFG dword of whichever block answered, kept for the report */
    /* Built once at the end of init, at the kernel's own limit for a description (512, in
     * gpu_module_state.describe - and the same number again here, because the shorter of the two is what
     * decides what a user reads).  The size is asserted by the host harness rather than trusted here: a
     * clipped tail would drop the engine inventory, which is the clause the next increment depends on. */
    char text[512];
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
/* A read of the user-mode page, through whichever mapping reaches it.  Counted in the same total as every
 * other read, because the number the driver reports has to be the number it issued. */
static u32 window_read(u32 offset)
{
    inv.reads++;
    if (inv.window_direct) return X->read32(inv.handle, offset);
    return X->read32(inv.window_handle, offset - NV_USERMODE_CFG0);
}

static u32 reg_read(u32 offset)
{
    inv.reads++;
    return X->read32(inv.handle, offset);
}

/* The documented way to read PTIMER (tu104 dev_usermode.ref.txt): TIME_1, TIME_0, then TIME_1 again,
 * repeating if the high half moved - otherwise a low-half overflow between the two reads could be taken
 * for a time up to four seconds in the future.  Following the note exactly is not ceremony: it is the
 * difference between a measured interval and a fabricated one. */
static int read_gpu_time(u64 *nanoseconds)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        u32 high = window_read(NV_USERMODE_TIME_1);
        u32 low = window_read(NV_USERMODE_TIME_0);
        u32 high_again = window_read(NV_USERMODE_TIME_1);
        if (high == high_again) {
            *nanoseconds = ((u64)high << 32) | low;
            return 1;
        }
    }
    return 0;
}

/* One device, told by the entries that describe it.  The published text fixes no order inside a device,
 * so a group is searched for the three kinds instead of being assumed to present them in sequence, and a
 * device that named no type is counted without a type rather than dropped. */
struct top_counts {
    u32 devices, decoded;
    u32 rows;                               /* rows read, the v2 format's own measure of effort */
    u32 lce, graphics, vic, copy, enc, dec, sec, gsp, jpg, other;
    u32 ce_pri, ce_inst, ce_runlist, ce_engine;
    int ce_found, ce_runlist_valid, ce_engine_valid;
};

static void top_count_device(struct top_counts *c, u32 type, u32 data, u32 en)
{
    if (type == NV_PTOP_TYPE_UNKNOWN && !data && !en) return;
    c->devices++;
    switch (type) {
    case NV_PTOP_TYPE_GRAPHICS: c->graphics++; break;
    case NV_PTOP_TYPE_COPY0: case NV_PTOP_TYPE_COPY1: case NV_PTOP_TYPE_COPY2: c->copy++; break;
    case NV_PTOP_TYPE_VIC: c->vic++; break;
    case NV_PTOP_TYPE_SEC: c->sec++; break;
    case NV_PTOP_TYPE_NVENC0: case NV_PTOP_TYPE_NVENC1: c->enc++; break;
    case NV_PTOP_TYPE_NVDEC: c->dec++; break;
    case NV_PTOP_TYPE_NVJPG: c->jpg++; break;
    case NV_PTOP_TYPE_GSP: c->gsp++; break;
    case NV_PTOP_TYPE_LCE:
        c->lce++;
        /* The first copy engine's own address and runlist are what a submission would have to name, so
         * they are taken while the group that carries them is still in hand.  Each field is used only if
         * that entry's valid bit says it applies: a clear valid bit is not a zero, it is "this chip told
         * me nothing", and reporting the difference is the whole reason the bit exists. */
        /* One device, one tuple.  The address, the instance and the two numbers all have to come from
         * the *same* engine, so the whole group is taken or not taken: claiming the first engine's
         * address and the second engine's runlist would describe a device that does not exist, which is
         * precisely the sort of inventary error a reader cannot see in a printed number. */
        if (!c->ce_found && (data || en)) {
            c->ce_found = 1;
            if (data) {
                c->ce_pri = NV_PTOP_DATA_PRI_BASE(data) << NV_PTOP_DATA_PRI_BASE_ALIGN;
                c->ce_inst = NV_PTOP_DATA_INST_ID(data);
            }
            if (en && NV_PTOP_ENUM_RUNLIST_VALID(en)) {
                c->ce_runlist = NV_PTOP_ENUM_RUNLIST(en);
                c->ce_runlist_valid = 1;
            }
            if (en && NV_PTOP_ENUM_ENGINE_VALID(en)) {
                c->ce_engine = NV_PTOP_ENUM_ENGINE(en);
                c->ce_engine_valid = 1;
            }
        }
        break;
    default: c->other++; break;      /* IOCTRL, the video codecs: named, not counted as an engine */
    }
}

/* The Blackwell entry, once its rows are joined: `lo' is bits 0..63 of the 96-bit entry, `hi' the third
 * row (bits 64..95).  A row of zero means the slot is invalid, per ROW_VALUE_INVALID, and a device with no
 * type is not a device, so nothing is counted for it.  IS_ENGINE is the entry's own word for "this device is
 * an engine that can be runlisted": it is the only vouching the two runlist fields get on this format, and
 * it is carried into the report as a valid bit rather than being silently believed. */
static void top_count_v2(struct top_counts *c, u64 lo, u64 hi)
{
    u32 type = NV_PTOP2_TYPE_ENUM(lo);

    if (!lo) return;          /* ROW_VALUE_INVALID: a slot that says nothing is not a device of type 0 */
    c->devices++;
    switch (type) {
    case NV_PTOP_TYPE_GRAPHICS: c->graphics++; break;
    case NV_PTOP_TYPE_COPY0: case NV_PTOP_TYPE_COPY1: case NV_PTOP_TYPE_COPY2: c->copy++; break;
    case NV_PTOP_TYPE_VIC: c->vic++; break;
    case NV_PTOP_TYPE_SEC: c->sec++; break;
    case NV_PTOP_TYPE_NVENC0: case NV_PTOP_TYPE_NVENC1: c->enc++; break;
    case NV_PTOP_TYPE_NVDEC: c->dec++; break;
    case NV_PTOP_TYPE_NVJPG: c->jpg++; break;
    case NV_PTOP_TYPE_GSP: c->gsp++; break;
    case NV_PTOP_TYPE_LCE:
        c->lce++;
        /* Same rule as the legacy walk: one device, one tuple, taken whole or not at all. */
        if (!c->ce_found) {
            c->ce_found = 1;
            c->ce_pri = NV_PTOP2_DEVICE_PRI_BASE(lo);
            c->ce_inst = NV_PTOP2_INSTANCE_ID(lo);
            c->ce_runlist = NV_PTOP2_RUNLIST_PRI_BASE(hi);
            c->ce_engine = NV_PTOP2_RLENG_ID(hi);
            c->ce_runlist_valid = c->ce_engine_valid = (int)NV_PTOP2_IS_ENGINE(lo);
        }
        break;
    default: c->other++; break;
    }
}

/* The v2 table, walked the way the chip describes it rather than the way a previous generation did.  The
 * CFG dword is read first because it is the block's self-report: if it does not say `version 2', no row is
 * read at all, and the answer is `that block is not there' rather than a walk over an aperture.  NUM_ROWS
 * from that dword bounds the read count, so a chip with a small table is not scanned past its end and a
 * chip with the published 353 rows is fully asked.  A run of sixteen invalid slots ends the walk early:
 * the table is dense at the front, and reading all 353 rows to confirm an empty tail would raise the
 * driver's own read count without learning a fact. */
static u32 top_walk_v2(u32 cfg, u32 rows_addr, struct top_counts *c)
{
    u32 devices, rows_per, rows, row = 0, invalid = 0;

    if (cfg == 0xffffffffu || cfg == 0u) return 0u;
    if (NV_PTOP_CFG_VERSION(cfg) != NV_PTOP_CFG_VERSION_DEVICE_INFO2) return 0u;
    devices = NV_PTOP_CFG_MAX_DEVICES(cfg);
    rows_per = NV_PTOP_CFG_MAX_ROWS_PER_DEVICE(cfg);
    rows = NV_PTOP_CFG_NUM_ROWS(cfg);
    if (!devices || devices > NV_PTOP_DEVICE_INFO2_SIZE_1) devices = NV_PTOP_DEVICE_INFO2_SIZE_1;
    if (!rows_per || rows_per > 8u) rows_per = 3u;
    if (!rows || rows > NV_PTOP_DEVICE_INFO2_SIZE_1) rows = NV_PTOP_DEVICE_INFO2_SIZE_1;
    for (u32 d = 0; d < devices && row < rows; d++) {
        u64 lo = 0, hi = 0;
        int open = 0;
        for (u32 r = 0; r < rows_per && row < rows; r++) {
            u32 v = reg_read(rows_addr + row * 4u);
            row++;                      /* every read consumes a row - and `break' below would otherwise
                                         * skip a for-increment and make the next device re-read this one */
            c->rows++;
            if (v == 0xffffffffu) return cfg;      /* past the end of what this BAR answers */
            if (!open) {
                if (!v) { if (++invalid >= 16u) return cfg; continue; }
                invalid = 0; open = 1; lo = 0; hi = 0;
            }
            if (r == 0) lo = v;
            else if (r == 1) lo |= (u64)v << 32;
            else hi = v;
            c->decoded++;
            if (!(v & NV_PTOP_CHAIN_BIT)) break;   /* ROW_CHAIN clear: this entry is complete */
        }
        if (open) top_count_v2(c, lo, hi);
    }
    return cfg;
}

/* One candidate table.  Entries belong to the same device while CHAIN is set, so a group is flushed when
 * that bit clears; a NOT_VALID row ends a group too.  An all-ones read ends the walk, because past the
 * end of a real BAR every register answers that way, and calling a field of ones a device list is the
 * worst thing this code could do: it would print an inventory the chip never gave. */
static void top_walk(u32 base, struct top_counts *c)
{
    u32 type = NV_PTOP_TYPE_UNKNOWN, data = 0, en = 0, empty_run = 0;
    int open = 0;

    for (u32 i = 0; i < NV_PTOP_DEVICE_INFO__SIZE_1; i++) {
        u32 v = reg_read(base + i * 4u);
        if (v == 0xffffffffu) { open = 0; return; }
        u32 kind = NV_PTOP_ENTRY(v);
        if (v == 0u || kind == NV_PTOP_ENTRY_NOT_VALID) {
            if (open) {
                top_count_device(c, type, data, en);
                open = 0; type = NV_PTOP_TYPE_UNKNOWN; data = en = 0;
            }
            /* Rows past the end of the table are NOT_VALID by definition.  Four of them before anything
             * has been decoded is an empty aperture, and reading the remaining sixty rows would raise the
             * read count this driver reports without learning a single fact about the chip. */
            if (++empty_run >= 4u) return;
            continue;
        }
        empty_run = 0;
        if (!open) { open = 1; type = NV_PTOP_TYPE_UNKNOWN; data = en = 0; }
        if (kind == NV_PTOP_ENTRY_DATA) data = v;
        else if (kind == NV_PTOP_ENTRY_ENUM) en = v;
        else if (kind == NV_PTOP_ENTRY_ENGINE_TYPE) type = NV_PTOP_TYPE_ENUM(v);
        c->decoded++;
        if (!(v & NV_PTOP_CHAIN_BIT)) {
            top_count_device(c, type, data, en);
            open = 0; type = NV_PTOP_TYPE_UNKNOWN; data = en = 0;
        }
    }
    if (open) top_count_device(c, type, data, en);
}

/* Ask the chip which engines it has, in the order its own headers describe.  Blackwell's format is tried
 * first - it is the one published for this generation, and its CFG says whether the block is there at all -
 * then the Turing and Ampere arrays, which are what a chip that predates the v2 layout will answer with.
 * Nothing is written.  A table that does not decode is reported as a table that did not decode, together with
 * what the two CFG dwords said, because "no copy engine" and "the block is gated against me" are different
 * facts and only one of them ends the submission work before it starts. */
static void probe_engines(void)
{
    struct top_counts best, cur;
    u32 layouts[4][2] = {
        { 0u, NV_PTOP_DEVICE_INFO_AMPERE },   /* PTOP0's rows, the v2 format, CFG read above */
        { 0u, NV_PTOP_DEVICE_INFO_ROWS1 },    /* PTOP1's rows, the same format */
        { 0u, NV_PTOP_DEVICE_INFO_TURING },                          /* legacy, Turing layout */
        { 0u, NV_PTOP_DEVICE_INFO_AMPERE },                          /* legacy, Ampere layout */
    };

    memset(&best, 0, sizeof best);
    inv.top_cfg = reg_read(NV_PTOP_DEVICE_INFO_CFG);
    if (inv.top_cfg == 0xffffffffu) inv.top_cfg = 0u;
    inv.top_cfg1 = reg_read(NV_PTOP_DEVICE_INFO_CFG1);
    if (inv.top_cfg1 == 0xffffffffu) inv.top_cfg1 = 0u;
    inv.top_cfg_devices = NV_PTOP_CFG_MAX_DEVICES(inv.top_cfg);
    inv.top_cfg_rows_per_device = NV_PTOP_CFG_MAX_ROWS_PER_DEVICE(inv.top_cfg);
    inv.top_cfg_rows = NV_PTOP_CFG_NUM_ROWS(inv.top_cfg);

    for (u32 which = 0u; which < 4u; which++) {
        memset(&cur, 0, sizeof cur);
        u32 cfg = 0u;
        if (which < 2u) {
            /* The CFG was read once above for the report; the walk is handed that value rather than
             * asking the chip again, because two reads for one number is two chances to disagree. */
            cfg = top_walk_v2(which ? inv.top_cfg1 : inv.top_cfg, layouts[which][1], &cur);
            if (!cfg) continue;
        } else {
            /* The older formats have no self-report to consult, so they are only ever reached after both
             * v2 blocks declined: on a Blackwell part those same addresses hold v2 rows, and decoding them
             * with Ampere masks is how an inventory of nonsense gets printed with a straight face.  The
             * `break' above is what guarantees that, not a test here. */
            top_walk(layouts[which][1], &cur);
        }
        /* Two devices, four accepted entries, and at least one naming an engine the published list knows:
         * below that what sits in front of the decoder is an aperture, not a table.  The gate is what makes
         * "the block moved" an honest answer instead of a wrong inventory. */
        if (cur.devices < 2u || cur.decoded < 4u) continue;
        if (cur.lce + cur.graphics + cur.vic + cur.copy + cur.enc + cur.dec + cur.sec +
            cur.gsp + cur.jpg == 0u) continue;
        if (which < 2u) {
            /* A v2 block that decodes is the block the chip's own header described: nothing further is
             * asked, and the older masks are never pointed at rows they cannot interpret. */
            best = cur;
            inv.top_layout = which + 1u;    /* 1 and 2 are v2: PTOP0 at 0x22800, PTOP1 at 0x32800 */
            inv.top_v2_cfg = cfg;
            break;
        }
        /* The legacy pair stays a comparison rather than a preference: on a Turing or Ampere part both
         * offsets can hold a table, and the one naming more devices is the one worth quoting. */
        if (cur.devices > best.devices) {
            best = cur;
            inv.top_layout = which + 1u;    /* 3 and 4 are the 0x22700 and 0x22800 legacy arrays */
            inv.top_v2_cfg = 0u;
        }
    }
    inv.top_devices = best.devices;   inv.top_decoded = best.decoded;
    inv.top_rows = best.rows;
    inv.top_lce = best.lce;           inv.top_graphics = best.graphics;
    inv.top_vic = best.vic;           inv.top_copy = best.copy;
    inv.top_enc = best.enc;           inv.top_dec = best.dec;
    inv.top_sec = best.sec;           inv.top_gsp = best.gsp;
    inv.top_jpg = best.jpg;           inv.top_other = best.other;
    inv.top_ce_pri = best.ce_pri;     inv.top_ce_inst = best.ce_inst;
    inv.top_ce_runlist = best.ce_runlist;   inv.top_ce_engine = best.ce_engine;
    inv.top_ce_runlist_valid = best.ce_runlist_valid;   inv.top_ce_engine_valid = best.ce_engine_valid;
    inv.top_ce_found = best.ce_found;
}

/* What the second measurement is for.  A Blackwell submission needs an addressable channel and a doorbell
 * to ring it through, and nothing published says where that block is on this generation.  So the chip is
 * asked instead: if the class register answers with a class number and the clock in the same page is
 * advancing, the window the Volta and Turing documentation describes is present on this silicon, and the
 * copy-engine work after this is addressable rather than speculative.  If it answers nothing, that is the
 * ceiling, measured.  Both answers are reported; neither is written to. */
static void probe_usermode(void)
{
    /* The documented page sits at the top of a 16 MiB register BAR, which is past the kernel's per-mapping
     * cap - so on a real card the register mapping stops short of it and the page is mapped on its own.
     * Which of the two was used is recorded rather than glossed, because "the chip answers nothing at that
     * address" and "this driver could not reach that address" are different facts, and only the first one
     * says anything about the chip. */
    if (inv.mapping_bytes >= NV_USERMODE_TIME_1 + 4u) {
        inv.window_direct = 1;
    } else {
        u64 mapped = 0;
        inv.window_handle = X->map(inv.physical + NV_USERMODE_CFG0, NV_USERMODE_WINDOW_BYTES, 0, &mapped);
        if (!inv.window_handle ||
            (mapped ? mapped : NV_USERMODE_WINDOW_BYTES) <
                NV_USERMODE_NOTIFY_CHANNEL_PENDING + 4u - NV_USERMODE_CFG0) {
            inv.window_handle = 0;
            inv.window_unreachable = 1;
            return;
        }
    }
    inv.usermode_class = NV_USERMODE_CFG0_CLASS_ID(window_read(NV_USERMODE_CFG0));
    inv.window_live = inv.usermode_class != 0x0000u && inv.usermode_class != 0xffffu;
    if (!inv.window_live) return;
    if (!read_gpu_time(&inv.timer_start)) return;
    if (X->spin) X->spin(4096);
    if (!read_gpu_time(&inv.timer_end)) return;
    inv.timer_ticking = inv.timer_end > inv.timer_start;
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
    /* Short on purpose: the kernel keeps 256 characters, and a sentence clipped at the end would hide
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
    if (inv.window_unreachable) {
        /* A separate answer, because it is a fact about this mapping and not about the chip: saying
         * "silent" here would report a device that has nothing there when the truth is that this driver
         * could not reach far enough to ask. */
        put_text(&b, "; window unreachable");
    } else if (!inv.window_live) {
        put_text(&b, "; window silent");
    } else {
        put_text(&b, "; window ");
        put_text(&b, "class 0x");
        put_hex(&b, inv.usermode_class, 4);
        put_text(&b, inv.usermode_class == NV_USERMODE_CLASS_ID_VOLTA_TURING ? "=published" : "");
        if (inv.timer_ticking) {
            u64 delta = inv.timer_end - inv.timer_start;
            u32 us = (u32)(delta / 1000u);
            put_text(&b, " clock +");
            put_dec(&b, us);
            put_text(&b, "us");
        } else {
            put_text(&b, " clock frozen");
        }
    }
    /* The inventory the chip itself gave.  It goes at the end of the description, after the identity and
     * the window, because those two decide whether this sentence is about the silicon or about the
     * mapping; and it is phrased in outcomes, since "3 LCE" on a screen the CPU is painting is the fact a
     * reader needs before deciding what to write next.  Length is not a reason to drop any of it: the
     * kernel's field is 512 bytes and the panel wraps. */
    if (!inv.top_layout) {
        put_text(&b, "; engine table not decoded: CFG 0x224fc=0x");
        put_hex(&b, inv.top_cfg, 8);
        put_text(&b, " (v0x");
        put_hex(&b, NV_PTOP_CFG_VERSION(inv.top_cfg), 1);
        put_text(&b, "), 0x324fc=0x");
        put_hex(&b, inv.top_cfg1, 8);
        put_text(&b, " (v0x");
        put_hex(&b, NV_PTOP_CFG_VERSION(inv.top_cfg1), 1);
        put_text(&b, "); neither block reports the DEVICE_INFO2 format, and the Turing and Ampere arrays "
                     "at those addresses named no engine either");
    } else {
        static const u32 base_of[4] = { NV_PTOP_DEVICE_INFO_AMPERE, NV_PTOP_DEVICE_INFO_ROWS1,
                                        NV_PTOP_DEVICE_INFO_TURING, NV_PTOP_DEVICE_INFO_AMPERE };
        int v2 = inv.top_layout <= 2u;
        put_text(&b, "; engines at 0x");
        put_hex(&b, base_of[inv.top_layout - 1u], 5);
        if (v2) {
            /* The table's own dimensions, as it reported them, plus how many rows were read to answer: a
               "0 engines" from a 353-row scan and from a 4-row scan are not the same finding. */
            put_text(&b, " v2 (");
            put_dec(&b, NV_PTOP_CFG_MAX_DEVICES(inv.top_v2_cfg));
            put_text(&b, " devices x ");
            put_dec(&b, NV_PTOP_CFG_MAX_ROWS_PER_DEVICE(inv.top_v2_cfg));
            put_text(&b, " rows of ");
            put_dec(&b, NV_PTOP_CFG_NUM_ROWS(inv.top_v2_cfg));
            put_text(&b, ", ");
            put_dec(&b, inv.top_rows);
            put_text(&b, " read): LCE ");
        } else put_text(&b, ": LCE ");
        put_dec(&b, inv.top_lce);
        put_text(&b, "/VIC ");    put_dec(&b, inv.top_vic);
        put_text(&b, "/GFX ");    put_dec(&b, inv.top_graphics);
        put_text(&b, "/ENC ");    put_dec(&b, inv.top_enc);
        put_text(&b, "/DEC ");    put_dec(&b, inv.top_dec);
        put_text(&b, "/SEC ");    put_dec(&b, inv.top_sec);
        put_text(&b, "/GSP ");    put_dec(&b, inv.top_gsp);
        put_text(&b, " of ");     put_dec(&b, inv.top_devices);
        put_text(&b, " devices");
        /* A copy engine present but unaddressed is a real answer, so the clause is printed for it with a
         * question mark rather than dropped: "no LCE" and "LCE, no PRI base in its entry" are different
         * things to find out, and only one of them ends the channel work before it starts. */
        if (inv.top_lce) {
            put_text(&b, ", LCE ");
            if (inv.top_ce_found) {
                /* v2 calls it a field, and so does this line: the entry's 18 bits are not an address
                   until something says what the low 12 are.  On the legacy format the field is shifted by
                   12 by definition, so there the number really is one. */
                put_text(&b, v2 ? "pri-field 0x" : "pri 0x");
                put_hex(&b, inv.top_ce_pri, 6);
                put_text(&b, " inst "); put_dec(&b, inv.top_ce_inst);
            } else put_text(&b, "pri ?");
            put_text(&b, " runlist ");
            if (inv.top_ce_runlist_valid) {
                if (v2) { put_text(&b, "0x"); put_hex(&b, inv.top_ce_runlist, 5); }
                else put_dec(&b, inv.top_ce_runlist);
            } else put_text(&b, "?");
            put_text(&b, " engine ");
            if (inv.top_ce_engine_valid) put_dec(&b, inv.top_ce_engine); else put_text(&b, "?");
        }
    }
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
    inv.usermode_class = 0;
    inv.mapping_bytes = 0;
    inv.window_handle = 0;
    inv.window_direct = inv.window_unreachable = 0;
    inv.window_live = inv.timer_ticking = 0;
    inv.timer_start = inv.timer_end = 0;
    inv.console_offset = 0;
    inv.top_layout = 0;
    inv.top_devices = inv.top_decoded = 0;
    inv.top_lce = inv.top_graphics = inv.top_vic = inv.top_copy = inv.top_enc = 0;
    inv.top_dec = inv.top_sec = inv.top_gsp = inv.top_jpg = inv.top_other = 0;
    inv.top_ce_pri = inv.top_ce_inst = inv.top_ce_runlist = inv.top_ce_engine = 0;
    inv.top_ce_found = inv.top_ce_runlist_valid = inv.top_ce_engine_valid = 0;
    inv.top_cfg = inv.top_cfg_devices = inv.top_cfg_rows_per_device = inv.top_cfg_rows = 0;
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
    inv.mapping_bytes = mapped ? mapped : inv.mapped_bytes;
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

    probe_usermode();
    probe_engines();
    build_text();
    /* Wide enough for the whole description plus the prefix: the window clause sits at the end of the
     * text, so a narrow log buffer would cut the one part this increment added. */
    char line[608];
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
    if (inv.window_live) {
        /* A second line for whoever reads the log to decide what to build next: it states the inference
         * and its limit, rather than leaving a reader to conclude that the doorbell had been tried. */
        static char second[208];
        struct buf t = { second, second + sizeof(second) - 1 };
        put_text(&t, "nvidia: doorbell at BAR0+0x810090 named, not written; ");
        put_text(&t, inv.timer_ticking ? "with a live clock in that page a channel can be attempted next"
                                       : "with the clock frozen no submission is attempted");
        *t.at = 0;
        note(second);
    }

    *out_ops = &ops;
    return 0;
}
