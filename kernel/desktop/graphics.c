/* Display adapter discovery and the acceleration decision.
 *
 * This file is presentation only: it asks the GPU subsystem what is plugged in and says
 * so plainly.  Two things must never be confused, and the wording below keeps them apart
 * on purpose.  (1) Naming a display adapter is exact PCI ID matching against the tables
 * a real driver binds by - read-only, and safe on the adapter that is currently showing
 * the console.  (2) Using that adapter, which needs ported engine code and does not
 * happen just because a chip was recognised.  Do not mistake GOP scanout for a GPU
 * rendering driver.  Sizing a BAR, resetting a device or modesetting the firmware
 * display can destroy the only usable console, so nothing here does any of that; see
 * kernel/drivers/gpu/gpu_detect.c for the invariant and its logged proof.
 */
#include "scos.h"
#include "boot.h"

/* The capacity gpu_report() is given for its per-adapter listing - one block per display function on the
 * machine, each with its link, family, upstream entry points and path, so the bound scales with how many
 * adapters somebody plugged in rather than with what any one of them said.  The report buffer the whole panel
 * is assembled in is SCOS_GRAPHICS_REPORT in scos.h, sized from the measured worst case; this one only has
 * to be big enough not to be the reason a line is missing, and the boot suites check that it is not. */
#define DETAIL_LIMIT 2560
#define FSP_TAIL_LIMIT 6144             /* the room the firmware and FSP paragraphs are built in before they
                                           * are appended, so their length is known rather than hoped for */
#define REPORT_LIST_LIMIT 4

static struct boot_framebuffer scanout;

void graphics_init(const struct boot_framebuffer *fb)
{
    scanout = *fb;
    gpu_init(fb);
}

struct writer {
    char *start;                       /* the buffer's first byte, so the tail can be measured from it */
    char *at;
    size_t left;
    size_t dropped;                    /* bytes put() could not write, and therefore has to be reported */
};

static void put(struct writer *w, const char *text)
{
    /* The lost bytes are counted, not swallowed.  Every long report in this tree used to be able to stop
     * mid-word in silence, and twice now the clause that fell off the end was the one saying what to do next
     * - so the writer, which is the only place that can see both the text and the space, keeps a tally that
     * the caller prints as a sentence.  A bounded buffer with a loud edge is a limit; a bounded buffer
     * without one is a lie about the hardware. */
    size_t n = 0;
    while (text[n]) n++;
    if (n + 1u > w->left) {
        size_t room = w->left > 1u ? w->left - 1u : 0u;
        for (size_t i = 0; i < room; i++) *w->at++ = text[i];
        w->left -= room;
        w->dropped += n - room;
    } else {
        while (*text) { *w->at++ = *text++; w->left--; }
    }
    *w->at = 0;
}


static void put_number(struct writer *w, unsigned value)
{
    char digits[12], out[12];
    int i = 0, n = 0;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (i) out[n++] = digits[--i];
    out[n] = 0;
    put(w, out);
}

/* Released at the end of every public report: the reservation it spends is why each entry point hands the
 * writer `capacity - REPORT_NOTE_BYTES' rather than `capacity'. */
#define REPORT_NOTE_BYTES 448

static void put_note(struct writer *w, size_t capacity)
{
    if (!w->dropped) return;
    w->left = capacity - (size_t)(w->at - w->start);
    put(w, "\nThe report above needed ");
    put_number(w, (unsigned)w->dropped);
    put(w, " more byte(s) than this ");
    put_number(w, (unsigned)capacity);
    put(w, "-byte buffer, so it ends here - which is the limit of the buffer and not of what was measured.  "
           "Everything above was printed from the chip's own replies and the driver's own reads; `graphics fsp' "
           "prints these paragraphs on their own, and the terminal keeps the whole thing in its scrollback.");
}


/* Checksums are quoted in hexadecimal because that is the base every tool that produces them prints in:
 * `crc32 file.bin' on the machine a person copies the file from, and the number has to be comparable by
 * eye or it is decoration. */
static void put_hex(struct writer *w, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char out[9];
    for (int i = 0; i < 8; i++) out[i] = digits[(value >> ((7 - i) * 4)) & 0xf];
    out[8] = 0;
    put(w, out);
}

/* One staged image, as far as the record goes: its size, its checksum, and what the walk found inside it.
     * Deliberately terse, because the panel buffer is a fixed thing shared with the module's own inventory
     * and every clause here is repeated once per staged file - the explanation of what a missing container
     * means belongs to the paragraph below, which says it once. */
static void put_firmware_role(struct writer *w, const struct gpu_firmware_state *fw, unsigned role,
                              int first)
{
    const struct gpu_firmware_role *r = &fw->role[role];
    char number[24];

    if (!first) put(w, "; ");
    put(w, gpu_firmware_role_name(role));
    put(w, " ");
    fmt_u32(number, r->bytes);
    put(w, number);
    put(w, " B");
    if (r->state == BOOT_FIRMWARE_STATE_UNREADABLE) { put(w, ", unreadable"); return; }
    put(w, " crc32 0x");
    put_hex(w, r->crc32);
    if (!(r->flags & (BOOT_FIRMWARE_ELF64 | BOOT_FIRMWARE_ELF32))) {
        put(w, ", no container");
        if (r->flags & BOOT_FIRMWARE_TRUNCATED) put(w, ", truncated");
        return;
    }
    put(w, (r->flags & BOOT_FIRMWARE_ELF32) ? ", ELF32 " : ", ELF64 ");
    fmt_u32(number, r->sections);
    put(w, number);
    put(w, "s");
    if (r->flags & BOOT_FIRMWARE_TRUNCATED) put(w, ", truncated");
    /* A 32-bit image is the FMC one, whose names and CRCs are this kernel's own check, not the boot stub's
     * two-name walk, so the RM image's .fwimage/.fwsignature expectations are not this clause's business. */
    if (r->flags & BOOT_FIRMWARE_ELF32) return;
    if (r->flags & BOOT_FIRMWARE_PAYLOAD) {
        put(w, " +.fwimage ");
        fmt_u32(number, r->payload_bytes);
        put(w, number);
    } else
        put(w, " no .fwimage");
    if (r->flags & BOOT_FIRMWARE_SIGNATURE) {
        put(w, " +");
        put(w, fw->signature_name[0] ? fw->signature_name : "sig?");
        put(w, " ");
        fmt_u32(number, r->signature_bytes);
        put(w, number);
    } else
        put(w, " no signature section");
}

/* What the held bytes contain, as this kernel read them.  Kept beside the paragraph above rather than folded
 * into it because they answer different questions: that one is about the disk, this one about memory this
 * machine is currently spending, and a card can be diagnosed from either without the other. */
static void put_firmware_image(struct writer *w, const struct gpu_firmware_state *fw)
{
    char number[24];
    unsigned held = 0, absent = 0, dropped = 0;

    if (!fw) return;
    if (!fw->parsed) {
        if (fw->staged)
            put(w, "GSP firmware image: nothing held in memory.\n");
        return;
    }
    for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
        if (fw->image[i].resident) continue;
        if (fw->image[i].bytes) dropped++;
        else absent++;
    }
    put(w, "GSP firmware image: ");
    put_number(w, fw->resident);
    put(w, " of ");
    put_number(w, (unsigned)GPU_FIRMWARE_ROLES);
    put(w, " held at 0x");
    put_hex(w, (uint32_t)fw->address);
    put(w, " (");
    fmt_u32(number, fw->resident_bytes);
    put(w, number);
    put(w, " B). ");
    for (unsigned i = 0, first = 1; i < GPU_FIRMWARE_ROLES; i++) {
        const struct gpu_firmware_image *image = &fw->image[i];
        if (!image->resident) continue;
        held++;
        put(w, first ? "" : "; ");
        first = 0;
        put(w, gpu_firmware_role_name(i));
        put(w, ": ");
        if (!image->checksum_matches) { put(w, "in memory does not match the disk checksum"); continue; }
        if (!image->elf64 && !image->elf32) { put(w, "no container"); continue; }
        put(w, image->elf64 ? "ELF64 " : "ELF32 ");
        fmt_u32(number, image->bytes);
        put(w, number);
        put(w, " B, ");
        fmt_u32(number, image->sections);
        put(w, number);
        put(w, " sections");
        if (image->elf64) {
            if (image->payload_bytes) {
                put(w, ", .fwimage ");
                fmt_u32(number, image->payload_bytes);
                put(w, number);
                put(w, " B at 0x");
                put_hex(w, image->payload_offset);
            } else
                put(w, ", no .fwimage");
            if (image->signature_bytes) {
                put(w, ", ");
                put(w, image->signature_name[0] ? image->signature_name : "signature");
                put(w, " ");
                fmt_u32(number, image->signature_bytes);
                put(w, number);
                put(w, " B at 0x");
                put_hex(w, image->signature_offset);
            }
        } else {
            put(w, image->fmc_template ? ", header exact" : ", header mismatched");
            put(w, image->fmc_sections ? ", sections legal" : ", sections rejected");
            fmt_u32(number, image->crc_failures);
            put(w, ", ");
            put(w, number);
            put(w, " sh_info/CRC32 mismatches (packaging)");
            if (image->fmc_named) {
                put(w, "; image ");
                fmt_u32(number, image->image_bytes);
                put(w, number);
                put(w, " B at 0x");
                put_hex(w, image->image_offset);
                put(w, ", hash ");
                fmt_u32(number, image->hash_bytes);
                put(w, number);
                put(w, ", publickey ");
                fmt_u32(number, image->publickey_bytes);
                put(w, number);
                put(w, ", signature ");
                fmt_u32(number, image->fmc_signature_bytes);
                put(w, number);
                put(w, " B");
            } else
                put(w, "; missing one of image/hash/publickey/signature");
        }
    }
    if (absent) {
        fmt_u32(number, absent);
        put(w, "; ");
        put(w, number);
        put(w, " absent");
    }
    if (dropped) {
        fmt_u32(number, dropped);
        put(w, "; ");
        put(w, number);
        put(w, " read, dropped: no pages");
    }
    (void)held;
    put(w, ".\n");
}


/* The firmware paragraph of the report, kept out of graphics_report()'s body because it has two states
 * worth reading whole - nothing staged, and something staged - rather than one sentence bolted onto the
 * end of another. */
static void put_firmware_report(struct writer *w, const struct gpu_firmware_state *fw)
{
    char number[24];
    unsigned missing = 0;

    if (!fw || !fw->staged) {
        static const char *const files[] = BOOT_FIRMWARE_ROLE_FILES;
        put(w, "GSP firmware: none on the volume.  Stage ");
        for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
            put(w, i ? ", " : "");
            put(w, files[i]);
        }
        put(w, " in \\SCOS\\ (make gsp-firmware-set).\n");
        return;
    }
    put(w, "GSP firmware: ");
    put_number(w, fw->staged);
    put(w, " of ");
    put_number(w, (unsigned)GPU_FIRMWARE_ROLES);
    put(w, " staged, ");
    fmt_u32(number, fw->total_bytes);
    put(w, number);
    put(w, " B, ");
    put(w, fw->complete ? "complete for the FSP path: " : "incomplete for the FSP path: ");
    for (unsigned i = 0, first = 1; i < GPU_FIRMWARE_ROLES; i++) {
        if (fw->role[i].state == BOOT_FIRMWARE_STATE_ABSENT) { missing++; continue; }
        put_firmware_role(w, fw, i, first);
        first = 0;
    }
    if (missing) {
        put(w, ". Missing: ");
        for (unsigned i = 0, first = 1; i < GPU_FIRMWARE_ROLES; i++) {
            if (fw->role[i].state != BOOT_FIRMWARE_STATE_ABSENT) continue;
            put(w, first ? "" : ", ");
            first = 0;
            put(w, gpu_firmware_role_name(i));
        }
    }
    put(w, ".\n");
}


/* What the card's own flight security processor said, and what was therefore asked of it.
 *
 * This is the paragraph that turns "no driver" into something a person can act on: on this generation the
 * register block that owns the coprocessor refuses a cold reader, and the only thing that answers before the
 * refusal lifts is the mailbox described in kernel/drivers/gpu/gpu_fsp.c.  Whether that mailbox answers is the
 * fact that decides whether any of the rest is reachable, so it goes on the panel rather than in a manual
 * command somebody has to be told to run.  Every number here is a register read this kernel performed or a
 * count it kept while doing so; where nothing was asked, the sentence says which of the reasons applies,
 * because "the panel is quiet" and "this machine has no NVIDIA function" are different answers.
 */
static void put_fsp_mailbox(struct writer *w, const struct gpu_fsp_state *f)
{
    char number[24];

    if (f->queue_probed) {
        put(w, " reply queue: wrote 0/0, read back 0x");
        put_hex(w, f->reset_head);
        put(w, "/0x");
        put_hex(w, f->reset_tail);
        put(w, f->queue_writable ? " (store lands)" : " (store refused: no door into the queue)");
    } else {
        put(w, " reply queue: reads 0x");
        put_hex(w, f->reset_head);
        put(w, "/0x");
        put_hex(w, f->reset_tail);
        put(w, " (answered, so nothing was written to ask)");
    }
    put(w, ". request: ");
    if (!f->queried && !f->send_blocked) {
        put(w, "not sent.\n");
        return;
    }
    fmt_u32(number, f->packet_bytes);
    put(w, number);
    put(w, " B in ");
    put_number(w, f->send_bursts);
    put(w, f->send_bursts == 1u ? " window" : " windows");
    put(w, ", read back: ");
    if (f->send_readback) put(w, "identical, so the channel holds the packet");
    else {
        put(w, "differs from the packet at dword ");
        put_number(w, f->send_mismatch);
        put(w, " (reported, not obeyed: the doorbell went out anyway)");
    }
    put(w, ". fw version ");
    put_number(w, f->ucode_version & 0x7ffu);
    put(w, ", fuse check 0x");
    put_hex(w, f->fuse_check);
    put(w, f->fuse_check ? " (not a pass)" : " (pass)");
    put(w, ". hwcfg2 0x1100f4 = 0x");
    put_hex(w, f->hwcfg2);
    put(w, f->gsp_readable ? "" : " (locked)");
    put(w, ", clock 0x9400 ");
    if (!f->timer_readable) put(w, "unmapped");
    else if (f->timer_refused) put(w, "refused");
    else if (f->timer_dead) put(w, "silent");
    else {
        put(w, f->timer_ticking ? "moving" : "still");
        put(w, " (0x");
        put_hex(w, f->timer_start);
        put(w, " to 0x");
        put_hex(w, f->timer_end);
        put(w, ")");
    }
    put(w, ", VF page ");
    if (!f->vf_readable) put(w, "unmapped");
    else {
        put(w, "leaf0 0x");
        put_hex(w, f->vf_leaf0);
        put(w, " top0 0x");
        put_hex(w, f->vf_top0);
        put(w, " scratch 0x");
        for (unsigned i = 0; i < 4u; i++) { if (i) put(w, "/0x"); put_hex(w, f->vf_scratch[i]); }
        put(w, " (");
        put_number(w, (unsigned)f->vf_answered);
        put(w, " answered, ");
        put_number(w, (unsigned)f->vf_refusals);
        put(w, " refused)");
    }
    put(w, ".\n");
}


/* The one line that decides everything else on this panel, at the front where it cannot be lost.
 *
 * Three reports in a row from real hardware have now come back ending in the middle of a word, every one of
 * them cut somewhere before the paragraph this summarizes - which made the one fact anybody needed, whether
 * the card's security processor is even awake, unreadable no matter how carefully it was written down.  So
 * each of its five answers is compressed here to a word, with the number it came from beside it, and this
 * goes first: a copy that survives 350 bytes survives this line, and a line that has to be read from a screen
 * can be read from a screen.  Nothing is introduced - every clause is the same state the paragraphs below
 * spell out, from the same fields, and where a paragraph explains, this one only reports. */
static void put_fsp_headline(struct writer *w, const struct gpu_fsp_state *f)
{
    const struct gpu_firmware_state *fw = gpu_firmware_state();

    put(w, "FSP: ");
    if (!f) { put(w, "no PCI display function to ask.\n"); return; }
    if (!f->nvidia) {
        put(w, "not asked: this display function is vendor 0x");
        put_hex(w, f->vendor);
        put(w, ", and every offset on this path is NVIDIA's.\n");
        return;
    }
    if (!f->attempted) {
        put(w, "not asked: the function at BAR0 0x");
        put_hex(w, (uint32_t)f->bar0);
        put(w, f->skipped == 2 ? " is above this kernel's device window\n" : " has no memory aperture\n");
        return;
    }
    /* Which row of the reference driver's table this chip is measured against, first, because it is the
     * shortest sentence that explains a refusal of a file that is not malformed. */
    if (f->arch_unknown) put(w, "architecture unreadable, cot sizes unknown, ");
    else if (!f->fsp_supported) {
        put(w, "no FSP mailbox for arch 0x");
        put_hex(w, f->arch);
        put(w, " (the reference driver has none for it), ");
    } else {
        put(w, "cot ");
        put_number(w, f->cot_hash_bytes);
        put(w, "/");
        put_number(w, f->cot_pkey_bytes);
        put(w, "/");
        put_number(w, f->cot_sig_bytes);
        put(w, " v");
        put_number(w, f->cot_version);
        put(w, ", ");
    }
    /* A 0xbadf.... reply is not a boot status, and printing it as one made the panel read as though the card
     * had answered "not booted" when nothing that could be decoded had been decoded. */
    put(w, "boot ");
    if ((f->boot_scratch & 0xffff0000u) == 0xbadf0000u) {
        put(w, "window refused the read 0x");
        put_hex(w, f->boot_scratch);
        put(w, f->booted ? ", reported booted anyway, " : ", nothing to wait for (waited ");
    } else {
        put(w, "0x");
        put_hex(w, f->boot_scratch);
        put(w, f->booted ? " booted, " : " not booted (waited ");
    }
    if (!f->booted) {
        if (f->wait_clocked) {
            put_number(w, (unsigned)f->boot_ms_used);
            put(w, " of ");
            put_number(w, (unsigned)f->boot_ms);
            put(w, " ms), ");
        } else {
            put_number(w, (unsigned)f->boot_polls);
            put(w, " looks), ");
        }
    }
    put(w, !f->fsp_supported ? "no packet sent: this generation has no FSP mailbox in the reference driver"
        : !f->mapped ? "windows unmapped, nothing read"
        : !f->mailbox_readable ? "mailbox refused, no request attempted"
        : !f->queried ? "mailbox answers, nothing asked"
        : !f->responded ? "query sent, no reply read"
        : "query answered");
    if (f->responded) {
        put(w, " err 0x");
        put_hex(w, f->query.error);
    } else if (f->send_blocked) {
        put(w, ", send blocked(");
        put_number(w, (unsigned)f->send_blocked);
        put(w, ")");
    }
    put(w, ", handover ");
    if (!f->fmc_seen) put(w, "no FMC on the volume");
    else if (!f->staged) {
        put(w, "not staged, defect ");
        put_number(w, (unsigned)f->cot_defect);
    }
    else if (!f->handover_attempted) put(w, "staged, query not answered cleanly");
    else if (!f->handover_sent) {
        put(w, "written, refused(");
        put_number(w, (unsigned)f->handover_blocked);
        put(w, ")");
    } else if (!f->handover.bytes) put(w, "sent, no reply");
    else {
        put(w, "sent, err 0x");
        put_hex(w, f->handover.error);
        {
            const char *name = gpu_fsp_flcn_name(f->handover.error);
            if (name) { put(w, " "); put(w, name); }
        }
    }
    put(w, ", lock 0x");
    put_hex(w, f->hwcfg2);
    if (f->lockdown < 0) {
        put(w, " unreadable");
    } else {
        /* The word, then the bit that means it: a reader who knows the register can check the claim, and a
         * reader who does not is not left to infer a lock from "the read was not zero". */
        put(w, f->lockdown ? " locked" : " released");
        put(w, " bit13=");
        put_number(w, (unsigned)(f->lockdown & 1));
    }
    if (f->handover_sent) {
        put(w, " after 0x");
        put_hex(w, f->hwcfg2_after);
    }
    put(w, ", clock 0x9400 ");
    put(w, !f->timer_readable ? "unmapped" : f->timer_refused ? "refused" : f->timer_dead ? "silent block"
          : f->timer_ticking ? "moving" : "still");
    put(w, ", VF page ");
    put(w, !f->vf_readable ? "unmapped" : f->vf_answered ? "answering" : f->vf_refusals ? "refused" : "empty");
    put(w, ", fw ");
    put_number(w, fw ? (unsigned)fw->staged : 0u);
    put(w, "/5, card reads ");
    put_number(w, (unsigned)f->reads);
    put(w, " writes ");
    put_number(w, (unsigned)f->writes);
    put(w, ". Full text: `graphics fsp`.\n");
}

static void put_fsp_report(struct writer *w, const struct gpu_fsp_state *f)
{
    char number[24];

    /* Nothing was asked: the headline two lines up already says which of the four reasons applies, and the
     * paragraph repeating it in different words was the first thing this report could lose. */
    if (!f || !f->nvidia || !f->attempted || !f->mapped) return;
    put(w, "FSP mailbox: ");
    put(w, "BAR0 0x");
    put_hex(w, (uint32_t)f->bar0);
    put(w, ", 5 windows, ");
    put_number(w, (unsigned)f->writes);
    put(w, " writes / ");
    put_number(w, (unsigned)f->reads);
    put(w, " reads (");
    put_number(w, f->refusal_reads);
    put(w, " refused 0xbadf4100).\n  arch 0x");
    put_hex(w, f->arch);
    if (f->arch_unknown)
        put(w, " unreadable, so the container sizes are unknown and nothing is sent");
    else if (!f->fsp_supported)
        put(w, " has no FSP entry in the reference driver, so no mailbox exists here");
    else {
        put(w, ": cot ");
        put_number(w, f->cot_hash_bytes);
        put(w, "/");
        put_number(w, f->cot_pkey_bytes);
        put(w, "/");
        put_number(w, f->cot_sig_bytes);
        put(w, " v");
        put_number(w, f->cot_version);
    }
    put(w, ". boot scratch 0xad00bc = 0x");
    put_hex(w, f->boot_scratch);
    if (f->booted)
        put(w, " (booted)");
    else if (f->wait_clocked) {
        put(w, " (not booted; waited ");
        put_number(w, (unsigned)f->boot_ms_used);
        put(w, " of ");
        put_number(w, (unsigned)f->boot_ms);
        put(w, " ms)");
    } else {
        put(w, " (not booted; waited ");
        put_number(w, (unsigned)f->boot_polls);
        put(w, " reads)");
    }
    put(w, ".\n");
    if (!f->fsp_supported || !f->mailbox_readable) {
        put(w, "GSP handover: ");
        put(w, f->fsp_supported ? "not attempted: the queue registers refused.\n"
                                : "not applicable on this generation.\n");
        return;
    }
    put_fsp_mailbox(w, f);

    /* The capability query: one line, because it is one question. */
    put(w, "  query: ");
    if (!f->queried) {
        if (f->send_blocked == 1) {
            put(w, "not sent, the queue never fell idle after ");
            put_number(w, (unsigned)f->polled);
            put(w, " looks.\n");
        } else if (f->send_blocked == 2) {
            put(w, "not sent, the queue registers refused after ");
            put_number(w, (unsigned)f->polled);
            put(w, " looks.\n");
        } else {
            put(w, "not sent.\n");
        }
    } else if (!f->responded) {
        put(w, "sent, no reply after ");
        put_number(w, (unsigned)f->reply_polls);
        put(w, " looks");
        if (f->query.oversize) {
            put(w, ". the FSP left ");
            put_number(w, (unsigned)f->query.oversize);
            put(w, " B in the channel, longer than this buffer");
        }
        put(w, ".\n");
    } else {
        fmt_u32(number, f->query.bytes);
        put(w, "reply ");
        put(w, number);
        put(w, " B, type 0x");
        put_hex(w, (uint32_t)f->query.type);
        put(w, ", command 0x");
        put_hex(w, f->query.command);
        put(w, ", error 0x");
        put_hex(w, f->query.error);
        put(w, ".\n");
    }

    /* The handover: what was asked, and the one measurement that would change. */
    put(w, "GSP handover: ");
    if (!f->fmc_seen) { put(w, "no FMC image on the volume.\n"); return; }
    if (!f->staged) {
        put(w, "not staged, defect ");
        put_number(w, (unsigned)f->cot_defect);
        put(w, ".\n");
        return;
    }
    if (!f->handover_attempted) { put(w, "staged, not attempted.\n"); return; }
    if (!f->handover_sent) {
        put(w, "not sent, refused(");
        put_number(w, (unsigned)f->handover_blocked);
        put(w, ").\n");
        return;
    }
    fmt_u32(number, GPU_FSP_PAYLOAD_BYTES);
    put(w, number);
    put(w, " B for the ");
    fmt_u32(number, (unsigned)f->image_bytes);
    put(w, number);
    put(w, " B FMC image at 0x");
    put_hex(w, (uint32_t)f->image_physical);
    put(w, " (CRC32 0x");
    put_hex(w, (uint32_t)f->image_crc32);
    put(w, ")");
    if (!f->libos_staged) {
        put(w, ", no LibOS record: no pages for one.\n");
        return;
    }
    put(w, ", LibOS record 0x");
    put_hex(w, (uint32_t)f->libos_phys);
    put(w, " (LOGINIT/LOGINTR/LOGRM 64 KiB each at 0x");
    put_hex(w, (uint32_t)f->libos_log_phys);
    put(w, ", RMARGS 4 KiB at 0x");
    put_hex(w, (uint32_t)f->libos_rmargs_phys);
    put(w, ")");
    if (f->queue_staged) {
        put(w, ", ring 0x");
        put_hex(w, (uint32_t)f->queue_phys);
        put(w, " (command and status, ");
        put_number(w, f->queue_ptes);
        put(w, " page(s) each way, status at 0x");
        put_hex(w, (uint32_t)f->queue_stat_phys);
        put(w, ")");
    }
    if (!f->params_staged) {
        put(w, "; no boot parameters staged: the resource manager image this volume does not carry is\n"
               "  what they describe, so the handover carried no descriptor and the FMC was told nothing.\n"
               "  boot: not attempted.\n");
        return;
    }
    put(w, ", boot args 0x");
    put_hex(w, (uint32_t)f->boot_params_phys);
    put(w, ".\n  boot: ");
    /* The outcome, then the registers it was read from - because the FMC reports success by going quiet and
     * releasing the lockdown, not by answering, and a log that only says "no reply" reads like a failure. */
    if (f->settled) {
        put(w, "settled in ");
        put_number(w, (unsigned)f->handover_ms_used);
        put(w, " ms, ");
        if (f->mbox_error) {
            put(w, "FMC mailbox error 0x");
            put_hex(w, (uint32_t)f->gsp_mailbox0_after);
            put(w, "/0x");
            put_hex(w, (uint32_t)f->gsp_mailbox1_after);
        } else if ((f->gsp_mailbox0_after || f->gsp_mailbox1_after) && f->mailbox_after_valid) {
            put(w, "mailbox names the boot args, lockdown released");
        } else {
            put(w, "lockdown bit 13 clear");
        }
    } else if (f->booted) {
        /* The contradiction this panel used to print without noticing it was one: the therm scratch says the
         * FSP finished booting, and the lockdown that boot releases never lifts.  Both cannot be true of a
         * chip that read the packet, so the pair is stated as what it is - a boot report with no handover
         * behind it - instead of being filed under "no answer". */
        put(w, "nothing in ");
        put_number(w, (unsigned)f->handover_ms_used);
        put(w, " of 4000 ms, though the scratch says the FSP booted: lockdown never lifted");
    } else {
        put(w, "no answer in ");
        put_number(w, (unsigned)f->handover_ms_used);
        put(w, " of 4000 ms");
    }
    if (f->handover.bytes) {
        fmt_u32(number, f->handover.bytes);
        put(w, "; reply ");
        put(w, number);
        put(w, " B error 0x");
        put_hex(w, f->handover.error);
        {
            const char *name = gpu_fsp_flcn_name(f->handover.error);
            if (name) { put(w, " ("); put(w, name); put(w, ")"); }
        }
    }
    /* The registers as they read, and the verdict word beside each: a zero and a 0xbadf... are printed rather
     * than hidden, and neither is called an answer. */
    put(w, ", hwcfg2 0x");
    put_hex(w, f->hwcfg2);
    put(w, " -> 0x");
    put_hex(w, f->hwcfg2_after);
    put(w, !f->hwcfg2_after_valid ? " (no answer)" : f->gsp_released ? " (lock released)"
                                                                   : " (still locked)");
    put(w, ", mailboxes 0x");
    put_hex(w, (uint32_t)f->gsp_mailbox0_after);
    put(w, "/0x");
    put_hex(w, (uint32_t)f->gsp_mailbox1_after);
    if (f->mbox_error) put(w, " (FMC error)");
    if (f->handover_submit_valid) {
        put(w, ", submit 0x");
        put_hex(w, f->handover_head);
        put(w, "/0x");
        put_hex(w, f->handover_tail);
        put(w, f->handover_consumed ? " (read)" : " (never read)");
        put(w, f->handover_tail == f->handover_doorbell ? ", tail holds 0x" : ", tail does NOT hold 0x");
        put_hex(w, f->handover_doorbell);
        /* The instant after the ring, which is the only reading that separates a dropped doorbell from a
         * consumed one: if the tail would not take the value at all, nothing downstream is about the FSP. */
        put(w, f->handover_db_landed ? ", doorbell landed 0x" : ", doorbell did NOT land 0x");
        put_hex(w, f->handover_db_tail);
        if (f->handover_retried) put(w, ", rung twice");
    }
    put(w, ", scratch 0x");
    put_hex(w, f->boot_scratch_after);
    put(w, ", queue ");
    put(w, f->queues_idle_after ? "idle" : "busy");
    if (f->handover_have_channel) {
        put(w, ", channel dword 0 after the wait 0x");
        put_hex(w, f->handover_emem0_after);
    }
    put(w, ".\n");
    /* What the channel did with the handover packet, in the numbers that separate the faults that all print as
     * "no reply": the arm and the offset it names, the four dwords the window hands back against the four that
     * went in, and where - if anywhere - the packet's own first dword is in the first kilobyte of the channel. */
    {
        const uint32_t *got = f->handover_have_channel ? f->handover_got : f->emem_got;
        const uint32_t *want = f->handover_have_channel ? f->handover_want : f->emem_want;
        uint32_t bytes = f->handover_have_channel ? f->handover.request_bytes : f->packet_bytes;
        uint32_t found = f->handover_have_channel ? f->handover_packet_at : f->packet_at;

        put(w, "  channel: arm 0x");
        put_hex(w, f->ememc_after);
        put(w, " naming offset 0x");
        put_hex(w, f->ememc_after & 0x3fffffu);
        put(w, " (last window 0x");
        put_hex(w, bytes > 0x100u ? ((bytes - 1u) & ~0xffu) : 0u);
        put(w, "); window returns");
        for (unsigned k = 0; k < 4u; k++) {
            /* Only the dwords the packet actually has: a twelve-byte packet has three, and printing a fourth
             * would be a comparison against a zero that was never written. */
            if (k * 4u + 4u > bytes) break;
            put(w, " 0x");
            put_hex(w, got[k]);
        }
        put(w, " where");
        for (unsigned k = 0; k < 4u; k++) {
            if (k * 4u + 4u > bytes) break;
            put(w, " 0x");
            put_hex(w, want[k]);
        }
        put(w, " went in; scan finds the packet at ");
        if (found == 0xffffffffu) put(w, "no offset in the first 1 KiB");
        else { put(w, "0x"); put_hex(w, found); }
        put(w, ".\n");
    }
    /* What RM itself said, which is the only reading here that comes from the resource manager rather than
     * from the falcon that was supposed to start it. */
    if (!f->queue_staged) {
        put(w, "  rm: no ring was staged, so it had nothing to answer through.\n");
        return;
    }
    put(w, "  rm: status queue write 0x");
    put_hex(w, f->queue_write_ptr);
    put(w, " read 0x");
    put_hex(w, f->queue_read_ptr);
    if (f->rpc_seen) {
        put(w, " - running, and its first request is RPC 0x");
        put_hex(w, f->rpc_function);
        put(w, ".\n");
    } else {
        put(w, " - nothing written: the ring this kernel built was never touched.\n");
    }
}


/* The firmware and security-processor paragraphs on their own.
 *
 * This exists because the whole panel is longer than what a person can carry out of a text console in one
 * paste: the last three reports were truncated somewhere inside the detection list, which is upstream of
 * exactly the paragraphs anybody needs to read.  A narrower surface, not shorter prose - the sentences below
 * are the same calls the panel makes, in the same order, so the two cannot disagree with each other. */
void graphics_fsp_report(char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    size_t room = capacity > REPORT_NOTE_BYTES ? capacity - REPORT_NOTE_BYTES : capacity;
    struct writer w = {.start = out, .at = out, .left = room, .dropped = 0};

    *out = 0;
    put(&w, "FSP and GSP firmware, in the words the boot panel prints:\n");
    put_fsp_headline(&w, gpu_fsp_state());
    put_firmware_report(&w, gpu_firmware_state());
    put_firmware_image(&w, gpu_firmware_state());
    put_fsp_report(&w, gpu_fsp_state());
    put_note(&w, capacity);
}

void graphics_report(char *out, size_t capacity)
{
    char number[24];
    const struct gpu_driver *engine = gpu_bound_driver();
    const struct gpu_device *owner = gpu_scanout_device();

    if (!out || capacity == 0) return;
    size_t room = capacity > REPORT_NOTE_BYTES ? capacity - REPORT_NOTE_BYTES : capacity;
    struct writer w = {.start = out, .at = out, .left = room, .dropped = 0};
    *out = 0;                     /* every write below goes through put(), which bounds and counts */

    /* Three states, described differently on purpose: an engine that paints the console, an engine
     * that works but sits on a function that does not feed the console, and no engine at all.  The
     * middle one is the state a virtual machine is usually in, and a report that called it either
     * "accelerated" or "unavailable" would be lying. */
    const struct gpu_module_state *ms = gpu_module_state();
    /* First, because every long report a person has copied out of this OS so far lost its end: the line below
     * the summary is the longest text on the panel and the first thing to fall off a clipboard. */
    put_fsp_headline(&w, gpu_fsp_state());
    put(&w, "Renderer: ");
    if (engine) put(&w, "CPU software compositor, engine available for selected ops\n");
    else if (ms && ms->bound && gpu_engine_drives_output())
        put(&w, "CPU software compositor, GPU engine painting flat output regions\n");
    else if (ms && ms->bound && ms->identification_only)
        put(&w, "CPU software compositor - the GPU is NOT rendering: the module that knows this chip "
                "implements no engine\n");
    else if (ms && ms->bound)
        put(&w, "CPU software compositor, GPU engine bound on a second PCI function\n");
    else put(&w, "CPU software compositor\n");
    put(&w, "Scanout: firmware GOP, PAT write-combining\n");
    put(&w, "GPU acceleration: ");
    if (engine) {
        put(&w, "engine bound for family ");
        put(&w, engine->family);
        put(&w, "\n");
    } else if (ms && ms->bound) {
        put(&w, "driver module ");
        put(&w, ms->name);
        put(&w, " (family ");
        put(&w, ms->family);
        put(&w, ") ");
        if (ms->identification_only) {
            /* No readback number, because nothing was asked of the card to read back: this module's
             * whole output is what the chip said about itself. */
            put(&w, "read this chip's own registers and reports what it found; it offers no engine "
                    "operation; ");
        } else {
            put(&w, "loaded from storage and verified by device readback, ");
            fmt_u32(number,(u32)ms->self_test_matches);put(&w,number);
            put(&w, "/");
            fmt_u32(number,(u32)ms->self_test_pixels);put(&w,number);
            put(&w, " pixels; ");
        }
        put(&w, gpu_engine_drives_output()
            ? "used for flat output regions"
            : (ms->identification_only ? "the CPU compositor still paints every pixel of this screen"
                                       : "not used for output: another PCI function feeds this display"));
        put(&w, "\n  opened \\SCOS\\");
        put(&w, ms->name);
        put(&w, ".MOD (");
        fmt_u32(number,(u32)ms->file_bytes);put(&w,number);
        put(&w, " B) of ");
        fmt_u32(number,ms->store_count);put(&w,number);
        put(&w, " module file(s) on the disk, resident ");
        fmt_u32(number,(u32)ms->resident_bytes);put(&w,number);
        put(&w, " B; the other families' ");
        fmt_u32(number,ms->unopened_bytes);put(&w,number);
        put(&w, " B were never read\n");
        /* What the driver read out of the chip, in the chip's own numbers, on the screen a user pastes.
         * It used to live only in `gpuinfo', which meant the one panel everyone reads reported that a
         * module had run while the answer it produced stayed one command away - and an answer nobody
         * pastes is an answer that does not get acted on.  This is also where a report of "the GPU is
         * not rendering" gets its reason: the identity line, the submission window and the engine
         * inventory, in the driver's own words, with the read count it issued. */
        if (ms->describe[0]) {
            put(&w, "Engine: ");
            put(&w, ms->describe);
            put(&w, "\n");
        }
    } else if (ms && ms->present) {
        put(&w, "module ");
        put(&w, ms->name);
        put(&w, " loaded but the engine failed its self-test and was detached\n");
    } else {
        put(&w, "unavailable (no driver module loaded for this chip)\n");
    }
    /* The boot stub is the only component that can tell "this disk holds no driver for your chip" apart
     * from "the driver is here but I could not read it", and after ExitBootServices nobody else can.  Its
     * verdict is passed through because the two cases want different answers from a user: put the file
     * for this family on the ESP, or nothing at all. */
    {
        const struct boot_handoff *h = kernel_boot_handoff();
        put(&w, "  module store: ");
        if (!h) put(&w, "unknown (the kernel booted without a UEFI handoff)\n");
        else if (h->module_state == 1) {
            put(&w, "staged from \\SCOS\\");
            put(&w, h->module_name);
            put(&w, ".MOD by the boot stub, ");
            fmt_u32(number, (u32)h->module_bytes);put(&w, number);
            put(&w, " B, payload checksum verified\n");
        } else if (h->module_state == 2) {
            put(&w, "the boot disk holds no module for the matched family\n");
        } else if (h->module_state == 3) {
            put(&w, "the stub could not read the module file\n");
        } else {
            /* The stub is the only witness to what its scan reached, and after ExitBootServices its counts
             * are the sole way this screen can tell "the bus holds no display function" from "the bus was
             * never looked at" - which is the difference between a machine with nothing to load and a
             * loader that stopped at a bridge. */
            put(&w, "not consulted: the read-only scan the stub ran inspected ");
            fmt_u32(number, h->pci_functions_seen); put(&w, number);
            put(&w, " function(s) on ");
            fmt_u32(number, h->pci_buses_scanned); put(&w, number);
            put(&w, " bus(es) and found ");
            fmt_u32(number, h->pci_display_functions); put(&w, number);
            if (h->pci_display_functions)
                put(&w, " display function(s), none claimed by a driver family in the binding table\n");
            else
                put(&w, " display function(s)\n");
        }
    }
    /* First line a helper asks for when a report and a repository disagree. */
    {
        char stamp[96];
        scos_build_stamp(stamp, sizeof(stamp));
        put(&w, "Build: ");
        put(&w, stamp);
        put(&w, scos_build_modified() ? "  (UNCOMMITTED tree - not evidence about any commit)\n"
                                      : "  (matches the committed source)\n");
    }
    /* Whether the scanning adapter's own registers were read.  "Identified" and "touched" are
     * different claims and this OS has until now been able to make only the first. */
    if (owner) {
        put(&w, "Device access: ");
        if (owner->reg_state == 1) {
            put(&w, "BAR0 of ");
            fmt_u32(number, (uint32_t)owner->bar[0]); put(&w, number);
            put(&w, " read through the kernel's device mapping; first dword 0x");
            fmt_u32(number, owner->reg_first); put(&w, number);
            /* Who read it matters as much as what came back.  The kernel's own probe is a fallback for a
             * machine whose module was never staged; a driver module reading the chip is a driver working.
             * The panel states which of the two happened instead of letting a raw dword imply either. */
            if (owner->reg_kernel_fallback)
                put(&w, " - by the kernel, because the store staged no module for this family");
            put(&w, "\n");
        } else if (owner->reg_state == 2) put(&w, "mapped, all-ones reads\n");
        else if (owner->reg_state == 3) put(&w, "BAR0 beyond the mapping capacity\n");
        else if (owner->reg_state == 4) put(&w, "memory decode disabled by firmware\n");
        else if (owner->reg_state == 5) put(&w, "BAR0 is the scanout aperture; already mapped once\n");
        else {
            /* "left to a driver module" used to be printed whether or not a module existed for this
             * machine, which is exactly how a chip ends up unread by anybody while the screen claims
             * someone is in charge of it.  The two cases now say which one it is. */
            const struct boot_handoff *acc = kernel_boot_handoff();
            if (acc && acc->module_state == 1) {
                /* "Device access: none" used to be the whole sentence, and on a machine whose driver
                 * module had in fact read twenty-odd registers of that chip it was read as "nobody has
                 * touched my card".  It is the kernel that stayed out, not everybody: the module reports
                 * its own reads a few lines above, and this line has to say whose access is absent. */
                const struct gpu_module_state *ms = gpu_module_state();
                put(&w, "none by the kernel");
                if (ms && ms->present) {
                    put(&w, " - the driver module ");
                    put(&w, ms->name);
                    put(&w, " reads this chip itself (counts above)");
                } else
                    put(&w, " (module staged, not resident: nothing read)");
                put(&w, "\n");
            } else
                put(&w, "none (no BAR0 the kernel could read, and the store staged no module)\n");
        }
    }
    put(&w, "Display: ");
    fmt_u32(number, scanout.width);
    put(&w, number);
    put(&w, "x");
    fmt_u32(number, scanout.height);
    put(&w, number);
    put(&w, "\n");

    if (!gpu_device_count()) {
        put(&w, "No PCI display function found; firmware scanout retained.\n");
    } else {
        /* Static and as large as the buffer the panel prints into: a local at 32 KiB is a stack overflow
         * waiting for a taller report, and 2560 bytes was a cliff the middle of a sentence fell off.  As a
         * `static' the size costs nothing on the stack and stops being a limit anyone has to remember to
         * raise; the report's own writer counts anything it cannot fit either way. */
        static char detail[SCOS_GRAPHICS_REPORT];
        gpu_report(detail, sizeof detail);
        put(&w, detail);
    }
    put(&w, "config writes during detection: ");
    put_number(&w, (unsigned)gpu_config_writes_during_detect());
    put(&w, "; no BAR sizing, no modeset, no GPU reset\n");
    /* Kept in the report for the same reason the loader keeps two tables: one number is "a driver in
     * this tree binds this id", the other is "we know what this chip is called".  Collapsing them into
     * one big rule count would let a name read like support. */
    {
        /* Where the work has to go through on this generation, and what this build does and does not do
         * about it.  Printed beside the numbers rather than in prose somewhere else, because the state of
         * the coprocessor is the thing a reader needs in order to agree or argue with the next step. */
        /* These three paragraphs whole or not at all, and the way to be sure of that is to build them first
         * and look at the length, because the alternative - starting to write and running out - is how every
         * long panel reported to this project so far came back cut somewhere with the answer at the end of it.
         * The scratch is the same size as the buffer the caller prints from, which is what `w.left' is measured
         * against; if a tail is ever longer than the scratch, that is said too, and both numbers are in the
         * report.  Nothing here truncates without saying so, and `graphics fsp' prints the same three paragraphs
         * from the same three functions, so the rest is always available. */
        {
            char tail[FSP_TAIL_LIMIT];
            struct writer tw = {.start = tail, .at = tail, .left = sizeof tail, .dropped = 0};
            size_t need;

            *tail = 0;
            put_firmware_report(&tw, gpu_firmware_state());
            put_firmware_image(&tw, gpu_firmware_state());
            put_fsp_report(&tw, gpu_fsp_state());
            need = strlen(tail);
            if (need + 1u <= w.left && need + 1u < sizeof tail) put(&w, tail);
            else {
                put(&w, "Firmware intake and FSP mailbox: not printed.  They need ");
                fmt_u32(number, (u32)need);
                put(&w, number);
                put(&w, need + 1u >= sizeof tail ? " bytes, which is more than this build's own tail buffer of "
                                                 : " bytes and this buffer has only ");
                if (need + 1u >= sizeof tail) {
                    fmt_u32(number, (u32)sizeof tail);
                    put(&w, number);
                    put(&w, " bytes to hold them - the paragraphs are longer than anything measured so far, and "
                             "that is worth reporting as its own finding");
                } else {
                    fmt_u32(number, (u32)(w.left - 1u));
                    put(&w, number);
                    put(&w, " left, which is not enough for them to be printed whole");
                }
                put(&w, ".  Run `graphics fsp`, which prints those paragraphs on their own and nothing else, and "
                        "report that instead: an ending that says where the rest is, with both numbers in it, is "
                        "not the same loss as a sentence that stops halfway.\n");
            }
        }
        const struct gpu_module_state *gsp = gpu_module_state();
        if (gsp && gsp->bound) {
            const struct gpu_fsp_state *fs = gpu_fsp_state();
            put(&w, "GSP path: ");
            if (fs && fs->handover_sent) put(&w, "handover written to the card this boot");
            else if (fs && fs->handover_attempted) put(&w, "handover attempted, no answer");
            else if (fs && fs->queried) put(&w, "query sent, no handover");
            else put(&w, "nothing written to the card this boot");
            put(&w, ".\n");
        }
    }
    put(&w, "Tables: ");
    put_number(&w, (unsigned)gpu_match_id_total());
    put(&w, " id rules bind a driver; ");
    put_number(&w, (unsigned)gpu_registry_id_total());
    put(&w, " further ids name a chip that no driver in this tree covers\n");
    if (owner) {
        put(&w, "Scanout owner ");
        put_number(&w, (unsigned)owner->bus);
        put(&w, ":");
        put_number(&w, (unsigned)owner->slot);
        put(&w, ".");
        put_number(&w, (unsigned)owner->function);
        put(&w, " identified from its BAR address alone.\n");
    }
    put_note(&w, capacity);
}
