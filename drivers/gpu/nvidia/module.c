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
    u32 top_gsp_field, top_gsp_reset, top_gsp_inst;   /* the coprocessor's own entry, which is what the base
                                         * unit below is checked against rather than assumed from */
    int top_gsp_found;
    u32 top_cfg, top_cfg_devices, top_cfg_rows_per_device, top_cfg_rows;
    u32 gsp_irqstat, gsp_engine;      /* NV_PGSP_FALCON_IRQSTAT / _ENGINE, as read */
    u32 gsp_mailbox0, gsp_mailbox1;   /* the two words a driver and the firmware leave for each other */
    u32 gsp_faultstat;                /* NV_PGSP_RISCV_FAULT_CONTAINMENT_SRCSTAT */
    int gsp_reachable;                /* the block answered at all, rather than every read being all-ones */
    int gsp_error_reads;              /* how many of the five answered with a PRI error code instead of a register */
    int gsp_locked;                   /* and all five answered the target-locked one: the block is shut to us */
    /* The other two targets in the same aperture, asked because the lock above is per target and not per
     * BAR.  `pri_shift' is the unit the chip confirmed for a v2 PRI base (0 = nothing confirmed, and then
     * no address is derived and nothing is probed), and `ce_*' is what the first copy engine's own block
     * said when it was asked - the block a submission would have to be built for. */
    u32 pri_shift, ce_base, ce_probe[2];
    int pri_confirmed;
    int ce_attempted;
    int ce_locked, ce_dead, ce_replies;   /* what the first copy engine's own block answered: the lock,
                                           * nothing at all, or a reply that is content */
    u32 fsp_scratch[4];
    int fsp_attempted, fsp_error_reads, fsp_locked, fsp_replies;
    int fsp_direct, fsp_unreachable;      /* reached through the register mapping, or not reached at all */
    u64 fsp_handle;                       /* its own page, when 0x8f0000 is past the mapping's cap */
    u32 top_cfg1;                     /* the second block's CFG, 0x324fc: asked, and reported if unanswered */
    u32 top_rows;                     /* rows actually read, so an inventory says how much was asked */
    u32 top_engines, top_bus;         /* entries the chip called engines, and the bus blocks it also lists */
    u32 top_ce_raw0, top_ce_raw1, top_ce_raw2;   /* the first copy engine's three rows, as read */
    u32 top_v2_cfg;                   /* the CFG dword of whichever block answered, kept for the report */
    /* Built once at the end of init, at the kernel's own limit for a description (1152, in
     * gpu_module_state.describe - and the same number again here, because the shorter of the two is what
     * decides what a user reads).  The size is asserted by the host harness rather than trusted here: the
     * fixture seeds the widest report the format can describe and checks the length, which is how 512
     * became 768 and why 768 became this: every clause that names a target the chip refused costs about a
     * hundred and thirty bytes, and a clipped tail would drop the very clause the next increment depends
     * on. */
    char text[1152];
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
    u32 engines;                            /* entries whose own IS_ENGINE bit is set: a device is not an
                                             * engine, and printing one as the other is how a chip gets
                                             * credit for blocks it cannot be given work on */
    u32 bus;                                /* PTOP's bus-side devices on this generation: PBUS, HSHUB,
                                             * HUBMMU, TMR - named so that the tally adds up */
    u32 lce, graphics, vic, copy, enc, dec, sec, gsp, jpg, other;
    u32 gsp_field, gsp_reset, gsp_inst;     /* the coprocessor's entry: the field whose unit can be checked */
    int gsp_found;
    u32 raw[3];                             /* the first copy engine's entry, exactly as the rows read */
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
    if (NV_PTOP2_IS_ENGINE(lo)) c->engines++;   /* the entry's own word for "this can be runlisted" */
    switch (type) {
    case NV_PTOP_TYPE_GRAPHICS: c->graphics++; break;
    case NV_PTOP_TYPE_COPY0: case NV_PTOP_TYPE_COPY1: case NV_PTOP_TYPE_COPY2: c->copy++; break;
    case NV_PTOP_TYPE_VIC: c->vic++; break;
    case NV_PTOP_TYPE_SEC: c->sec++; break;
    case NV_PTOP_TYPE_NVENC0: case NV_PTOP_TYPE_NVENC1: c->enc++; break;
    case NV_PTOP_TYPE_NVDEC: c->dec++; break;
    case NV_PTOP_TYPE_NVJPG: c->jpg++; break;
    case NV_PTOP_TYPE_GSP:
        c->gsp++;
        /* The coprocessor's own entry is kept for a second reason than the copy engine's is: its block's
         * absolute address *is* published, so this field is the one whose scaling can be tested instead of
         * assumed, and through it the whole table's unit.  Same one-device-one-tuple rule as above. */
        if (!c->gsp_found) {
            c->gsp_found = 1;
            c->gsp_field = NV_PTOP2_DEVICE_PRI_BASE(lo);
            c->gsp_reset = NV_PTOP2_RESET_ID(lo);
            c->gsp_inst = NV_PTOP2_INSTANCE_ID(lo);
        }
        break;
    case NV_PTOP_TYPE_PBUS: case NV_PTOP_TYPE_HSHUB:
    case NV_PTOP_TYPE_HUBMMU: case NV_PTOP_TYPE_TMR:
        c->bus++; break;
    case NV_PTOP_TYPE_LCE:
        c->lce++;
        /* Same rule as the legacy walk: one device, one tuple, taken whole or not at all. */
        if (!c->ce_found) {
            c->ce_found = 1;
            /* And the entry itself, uninterpreted, kept for the report: the field positions for this
             * format come from a header, not from a manual, so the raw rows are the only way a reader can
             * check the decode without owning the same chip. */
            c->raw[0] = (u32)lo;
            c->raw[1] = (u32)(lo >> 32);
            c->raw[2] = (u32)hi;
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
    inv.top_engines = best.engines;         inv.top_bus = best.bus;
    inv.top_ce_raw0 = best.raw[0];  inv.top_ce_raw1 = best.raw[1];  inv.top_ce_raw2 = best.raw[2];
    inv.top_gsp_field = best.gsp_field; inv.top_gsp_reset = best.gsp_reset;
    inv.top_gsp_inst = best.gsp_inst;   inv.top_gsp_found = best.gsp_found;
}

/* Is the coprocessor this generation runs its engines through reachable, and is it up?  Five reads of
 * registers NVIDIA publishes for gb100, and no write among them.  The reset status says whether the
 * processor is out of reset; the fatal and containment bits say whether it is complaining; the two mailboxes
 * are the words a driver and the firmware leave each other, which is where any handoff would begin.  If the
 * block answers, the next step of this path has something to be attempted against, and if it answers with
 * all ones, that is the ceiling and it gets reported as the ceiling rather than worked around. */
static void probe_gsp(void)
{
    inv.gsp_irqstat = reg_read(NV_PGSP_FALCON_IRQSTAT);
    inv.gsp_engine = reg_read(NV_PGSP_FALCON_ENGINE);
    inv.gsp_mailbox0 = reg_read(NV_PGSP_FALCON_MAILBOX0);
    inv.gsp_mailbox1 = reg_read(NV_PGSP_FALCON_MAILBOX1);
    inv.gsp_faultstat = reg_read(NV_PGSP_RISCV_FAULT_SRCSTAT);
    inv.gsp_reachable = !(inv.gsp_engine == 0xffffffffu && inv.gsp_irqstat == 0xffffffffu);
    /* How many of the five came back as an error code rather than as content.  One would be odd enough to
     * report on its own; all five, at five offsets whose reset values differ from each other, is the block
     * refusing the reader, which is a different fact and a different next step. */
    u32 words[5] = { inv.gsp_irqstat, inv.gsp_engine, inv.gsp_mailbox0, inv.gsp_mailbox1, inv.gsp_faultstat };
    for (unsigned i = 0; i < 5u; i++)
        if (NV_PRI_IS_ERROR(words[i])) inv.gsp_error_reads++;
    inv.gsp_locked = inv.gsp_error_reads == 5;
}

/* The FSP's scratch words, read from wherever they can be reached: the register mapping covers them on a
 * chip whose BAR the kernel mapped whole, and on the common 8 MiB-capped case they are given a page of their
 * own, exactly as the submission window is.  Which of the two it was is recorded, because "no reply at that
 * address" and "this driver could not reach that address" are different facts and only the first one says
 * anything about the chip. */
static u32 fsp_read(u32 offset)
{
    if (inv.fsp_direct) return reg_read(offset);
    inv.reads++;
    return X->read32(inv.fsp_handle, offset - (NV_PFSP_SCRATCH_GROUP_2_0 & ~0xfffu));
}

/* The two questions the locked coprocessor leaves open, asked of the two other targets in the same BAR.
 *
 * Both exist because the lock above is per PRIV target, not per aperture, and because the table the chip
 * published can be *checked* rather than interpreted: the GSP entry's PRI base field has no stated unit in
 * any source, but the address of the GSP's registers is published in absolute terms, so the one field whose
 * block is known becomes the experiment that measures the other engines' addresses.  If it does not land on
 * the known base, the unit stays unknown, no address is built from the table, and no read is attempted
 * below - an unconfirmed scaling is a guess with the confidence of a number, and the reads this driver
 * counts have to be reads it had a reason to issue. */
static void probe_targets(void)
{
    u32 words[4];

    if (inv.top_layout >= 1u && inv.top_layout <= 2u && inv.top_gsp_found && inv.top_gsp_field) {
        for (u32 shift = 0u; shift <= 12u; shift += 4u) {
            if (NV_PTOP2_PRI_UNIT(inv.top_gsp_field, shift) != NV_PGSP_BASE) continue;
            inv.pri_shift = shift;
            inv.pri_confirmed = 1;
            break;
        }
    }
    /* `ce_probe' is asked one question - does the block reply - and is deliberately not decoded: for this
     * generation the copy engine's register file publishes no offsets at all (gb100's dev_ce_base.h is
     * four lines about engine sharing), so a field extracted from its first two dwords would be a name
     * hung on a number nobody can check. */
    if (inv.pri_confirmed && inv.top_ce_found && inv.top_ce_pri) {
        inv.ce_base = NV_PTOP2_PRI_UNIT(inv.top_ce_pri, inv.pri_shift);
        if (inv.ce_base + 8u <= inv.mapping_bytes) {
            inv.ce_attempted = 1;
            inv.ce_probe[0] = reg_read(inv.ce_base);
            inv.ce_probe[1] = reg_read(inv.ce_base + 4u);
            words[0] = inv.ce_probe[0]; words[1] = inv.ce_probe[1];
            int errors = 0, dead = 0;
            for (unsigned i = 0; i < 2u; i++) {
                if (NV_PRI_IS_ERROR(words[i])) errors++;
                if (NV_PRI_IS_DEAD(words[i])) dead++;
            }
            inv.ce_locked = errors == 2;
            inv.ce_dead = dead == 2;
            inv.ce_replies = errors + dead < 2;
        }
    }
    /* The FSP's scratch words, which is as far as this goes: their contents are debug state that only the
     * firmware's own boot sequence interprets, and nothing published says what a particular value means.
     * The reply's shape is the whole question - is the block the vendor waits on reachable to us at all? */
    /* Asked only on a chip that decoded a v2 table, because 0x8f0320 is published for this generation and
     * no other: pointing it at a Turing BAR would read somebody else's register and call it the FSP's. */
    if (inv.top_layout >= 1u && inv.top_layout <= 2u) {
        u64 page = NV_PFSP_SCRATCH_GROUP_2_0 & ~0xfffu;
        u32 span = (NV_PFSP_SCRATCH_GROUP_2_0 - (u32)page) + NV_PFSP_SCRATCH_GROUP_2_WORDS * 4u;
        u64 mapped = 0;
        if (NV_PFSP_SCRATCH_GROUP_2_0 + NV_PFSP_SCRATCH_GROUP_2_WORDS * 4u <= inv.mapping_bytes) {
            inv.fsp_direct = 1;
        } else {
            inv.fsp_handle = X->map(inv.physical + page, 0x1000u, 0, &mapped);
            if (!inv.fsp_handle || (mapped ? mapped : 0x1000u) < span) {
                inv.fsp_handle = 0;
                inv.fsp_unreachable = 1;      /* this driver's bound, and it gets reported as that */
            }
        }
        inv.fsp_attempted = !inv.fsp_unreachable;
        for (unsigned i = 0u; inv.fsp_attempted && i < NV_PFSP_SCRATCH_GROUP_2_WORDS; i++) {
            words[i] = fsp_read(NV_PFSP_SCRATCH_GROUP_2_0 + i * 4u);
            inv.fsp_scratch[i] = words[i];
            if (NV_PRI_IS_ERROR(words[i])) inv.fsp_error_reads++;
        }
        inv.fsp_locked = inv.fsp_error_reads == (int)NV_PFSP_SCRATCH_GROUP_2_WORDS;
        inv.fsp_replies = inv.fsp_error_reads == 0 &&
                          !(words[0] == 0xffffffffu && words[1] == 0xffffffffu);
    }
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
     * kernel's field is 1152 bytes and the panel wraps. */
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
        put_text(&b, "/JPG ");    put_dec(&b, inv.top_jpg);
        put_text(&b, " of ");     put_dec(&b, inv.top_devices);
        put_text(&b, " devices");
        if (inv.top_layout <= 2u) {
            /* Only the v2 format has an IS_ENGINE bit to quote.  Printing a zero for the older ones would
             * claim the chip had called nothing an engine, when in fact it never said either way. */
            put_text(&b, ", ");
            put_dec(&b, inv.top_engines);
            put_text(&b, inv.top_engines == 1u ? " engine per its own IS_ENGINE, "
                                                : " engines per its own IS_ENGINE, ");
            put_dec(&b, inv.top_bus);
            put_text(&b, " bus");
        }
        if (inv.top_layout <= 2u && inv.top_ce_found) {
            /* The first copy engine's three rows, exactly as read.  The field positions above come from a
             * header and not from a manual, so the words alone could be a decode that flatters itself:
             * with the rows printed, anyone holding the same header can check the extraction without
             * owning the card. */
            put_text(&b, "; LCE rows 0x"); put_hex(&b, inv.top_ce_raw0, 8);
            put_text(&b, "/0x"); put_hex(&b, inv.top_ce_raw1, 8);
            put_text(&b, "/0x"); put_hex(&b, inv.top_ce_raw2, 8);
        }
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
    /* What the measurements above mean for the pixels on this screen, stated from the same numbers so it
     * cannot outlive them.  Naming engines and reaching them are different things: a Blackwell part takes
     * work through a channel in a window it lets a driver ring, and whether that window answered is a
     * reading, not an opinion. */
    /* The management processor, in the state it was found in.  This is reported ahead of any plan that
     * depends on it, because a plan built on a coprocessor nobody checked is the mistake this driver has
     * been careful about from the first line. */
    put_text(&b, "; GSP ");
    if (!inv.gsp_reachable) {
        put_text(&b, "block at 0x110000 unreachable (engine reads 0x");
        put_hex(&b, inv.gsp_engine, 8);
        put_text(&b, ")");
    } else if (inv.gsp_locked) {
        /* The value is the finding.  Naming it as an error code instead of as register content a mask can
         * happily slice up is what this branch exists to get right: an earlier build printed "reset status
         * 0x1" off exactly this reply on exactly this chip, which was a state invented out of a refusal. */
        /* Compressed on purpose, and the log line below carries the explanation: the panel has to hold the
         * whole report for a card whose identity line is longer than any the fixture seeds, and a clipped
         * tail would cut the clauses after this one. */
        put_text(&b, "block at 0x110000 refused all five reads (0x");
        put_hex(&b, inv.gsp_engine, 8);
        put_text(&b, ": a PRIV target locked against this reader, not a register value, so no field of it "
                     "is read)");
    } else {
        /* An all-ones read is no answer, and neither is an error code: the fatal and containment flags are
         * only quoted from registers that actually replied. */
        u32 st = NV_PRI_IS_DEAD(inv.gsp_engine) ? 0xffu
                                                : NV_PGSP_FALCON_ENGINE_RESET_STATUS(inv.gsp_engine);
        put_text(&b, "engine ");
        if (st == NV_PGSP_FALCON_ENGINE_RESET_DEASSERTED) put_text(&b, "out of reset");
        else if (st == NV_PGSP_FALCON_ENGINE_RESET_ASSERTED) put_text(&b, "in reset");
        else { put_text(&b, "reset status 0x"); put_hex(&b, st, 2); }
        put_text(&b, ", mailboxes 0x"); put_hex(&b, inv.gsp_mailbox0, 8);
        put_text(&b, "/0x"); put_hex(&b, inv.gsp_mailbox1, 8);
        if (!NV_PRI_IS_DEAD(inv.gsp_irqstat) && !NV_PRI_IS_ERROR(inv.gsp_irqstat) &&
            NV_PGSP_FALCON_IRQSTAT_FATAL(inv.gsp_irqstat))
            put_text(&b, ", fatal error flagged");
        if (!NV_PRI_IS_DEAD(inv.gsp_faultstat) && !NV_PRI_IS_ERROR(inv.gsp_faultstat) &&
            NV_PGSP_RISCV_FAULT_GLOBAL(inv.gsp_faultstat))
            put_text(&b, ", global memory faulted");
    }
    /* The unit of the table's PRI bases, decided by the one entry whose block has a published address.
     * Naming the entry that decided it is the point: it is what separates this number from an assumption
     * about an 18-bit field no manual documents, and anyone holding dev_gsp.h can check it. */
    if (inv.pri_confirmed) {
        put_text(&b, "; base unit measured on this chip: the GSP entry's field 0x");
        put_hex(&b, inv.top_gsp_field, 6);
        put_text(&b, " x ");
        put_dec(&b, 1u << inv.pri_shift);
        put_text(&b, " is the published 0x110000");
    } else if (inv.top_gsp_found) {
        put_text(&b, "; no base unit: the GSP entry's field 0x");
        put_hex(&b, inv.top_gsp_field, 6);
        put_text(&b, " scales to no candidate that reaches the published 0x110000");
    }
    if (inv.ce_attempted) {
        put_text(&b, "; the first LCE's block at 0x");
        put_hex(&b, inv.ce_base, 6);
        if (inv.ce_locked) put_text(&b, " refused the same way");
        else if (inv.ce_dead) put_text(&b, " answers nothing");
        else if (inv.ce_replies) {
            put_text(&b, " answers 0x"); put_hex(&b, inv.ce_probe[0], 8);
            put_text(&b, "/0x"); put_hex(&b, inv.ce_probe[1], 8);
            put_text(&b, ", which is content");
        }
    }
    if (inv.fsp_locked) {
        put_text(&b, "; the FSP scratch at 0x8f0320 refused too, so the microcontroller that releases the");
        put_text(&b, " lock is out of reach as well");
    } else if (inv.fsp_replies) {
        put_text(&b, "; the FSP scratch at 0x8f0320 answers 0x");
        for (unsigned i = 0u; i < NV_PFSP_SCRATCH_GROUP_2_WORDS; i++) {
            if (i) put_text(&b, "/0x");
            put_hex(&b, inv.fsp_scratch[i], 8);
        }
        put_text(&b, " (its own debug words, uninterpreted here)");
    } else if (inv.fsp_unreachable) {
        put_text(&b, "; the FSP page at 0x8f0000 could not be mapped, so nothing is claimed about it");
    }
    if (inv.top_lce && !inv.window_live) {
        put_text(&b, "; engines are register blocks, not channels: no submission window answered, so "
                     "nothing was submitted");
    } else if (inv.top_lce && inv.window_live && !inv.timer_ticking) {
        put_text(&b, "; the window answered but its clock did not tick, so nothing was submitted through it");
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
    inv.top_gsp_field = inv.top_gsp_reset = inv.top_gsp_inst = 0;
    inv.top_gsp_found = 0;
    inv.pri_shift = inv.ce_base = inv.ce_probe[0] = inv.ce_probe[1] = 0;
    inv.pri_confirmed = inv.ce_attempted = 0;
    inv.ce_locked = inv.ce_dead = inv.ce_replies = 0;
    inv.gsp_error_reads = inv.gsp_locked = 0;
    inv.fsp_scratch[0] = inv.fsp_scratch[1] = inv.fsp_scratch[2] = inv.fsp_scratch[3] = 0;
    inv.fsp_attempted = inv.fsp_error_reads = inv.fsp_locked = inv.fsp_replies = 0;
    inv.fsp_direct = inv.fsp_unreachable = 0;
    inv.fsp_handle = 0;
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
    probe_gsp();
    probe_targets();
    build_text();
    /* Wide enough for the whole description plus the prefix: the window clause sits at the end of the
     * text, so a narrow log buffer would cut the one part this increment added. */
    char line[1280];  /* the description plus the log prefix, at the wider budget */
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
    /* The coprocessor gets its own line, because on this generation the work goes through it and a reader
     * deciding what to build next needs its state, not an inference from the mailbox addresses. */
    {
        static char third[320];
        struct buf u = { third, third + sizeof(third) - 1 };
        put_text(&u, "nvidia: GSP block at BAR0+0x110000 read, nothing written; ");
        if (!inv.gsp_reachable)
            put_text(&u, "the block does not answer, so there is nothing to hand work to yet");
        else if (inv.gsp_locked) {
            /* The sentence a reader needs next is not "the coprocessor is down" but "the coprocessor will
             * not answer this reader", and the difference says what could change it: in the vendor's own
             * driver the same value is the thing it waits for the FSP to clear. */
            put_text(&u, "every offset answered 0xbadf4100, the code its own driver waits for the FSP to");
            put_text(&u, " clear: a door the boot firmware holds, not a processor to talk to");
        }
        else if (inv.gsp_engine == 0xffffffffu)
            put_text(&u, "and only some of its registers replied, so no state is claimed for it");
        else if (NV_PGSP_FALCON_ENGINE_RESET_STATUS(inv.gsp_engine) ==
                 NV_PGSP_FALCON_ENGINE_RESET_DEASSERTED)
            put_text(&u, "it is out of reset, so a firmware handoff has a running processor to speak to");
        else if (NV_PGSP_FALCON_ENGINE_RESET_STATUS(inv.gsp_engine) ==
                 NV_PGSP_FALCON_ENGINE_RESET_ASSERTED)
            put_text(&u, "it is held in reset, so starting it is the first thing any GSP path has to do");
        else
            put_text(&u, "its reset status is a value no published name covers");
        *u.at = 0;
        note(third);
    }
    /* One line per target is not enough once the reader is being refused: what a next increment can attempt
     * depends on whether *any* of the aperture answers, and the three answers only mean anything together. */
    if (inv.ce_attempted || inv.fsp_attempted || (inv.top_gsp_found && !inv.pri_confirmed)) {
        static char fourth[448];
        struct buf w = { fourth, fourth + sizeof(fourth) - 1 };
        int said = 0;
        put_text(&w, "nvidia: the other targets in the same aperture: ");
        if (inv.ce_attempted) {
            if (inv.ce_locked) put_text(&w, "the first LCE's own block refuses the way the GSP does");
            else if (inv.ce_dead) put_text(&w, "the first LCE's own block answers nothing");
            else if (inv.ce_replies) {
                put_text(&w, "the first LCE's own block replies with content, which is the first live ");
                put_text(&w, "engine register read on this chip and the one door worth trying next");
            }
            said = 1;
            put_text(&w, "; ");
        } else if (inv.top_gsp_found && !inv.pri_confirmed) {
            put_text(&w, "no engine block was asked, because the table's base field scaled to no address ");
            put_text(&w, "the published blocks agree with");
            said = 1;
            put_text(&w, "; ");
        }
        if (inv.fsp_attempted) {
            put_text(&w, said ? "and the FSP" : "the FSP");
            if (inv.fsp_locked)
                put_text(&w, " whose release the lock waits for does not answer this reader either");
            else if (inv.fsp_replies) {
                put_text(&w, "'s scratch words do reply, though they are its debug state and no conclusion");
                put_text(&w, " about the lock is drawn from reading them");
            } else
                put_text(&w, "'s scratch words answer nothing");
        } else if (inv.fsp_unreachable) {
            if (said) put_text(&w, "and ");
            put_text(&w, "this driver could not map the page the FSP's words live in, so nothing is ");
            put_text(&w, "claimed about it");
        }
        *w.at = 0;
        note(fourth);
    }

    *out_ops = &ops;
    return 0;
}
