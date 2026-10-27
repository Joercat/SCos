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
static void put_firmware_role(struct writer *w, const struct gpu_firmware_state *fw, unsigned role)
{
    const struct gpu_firmware_role *r = &fw->role[role];
    char number[24];

    if (role) put(w, "; ");
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
    unsigned held = 0, skipped = 0;

    if (!fw) return;
    if (!fw->parsed) {
        if (fw->staged)
            put(w, "GSP firmware image: the sizes and checksums above came from files the boot stub read and "
                   "then released, so nothing is held to read contents out of - either the volume offered "
                   "nothing worth keeping, or the allocation was refused, which is the firmware's answer and "
                   "not this OS choosing to forget.\n");
        return;
    }
    put(w, "GSP firmware image: ");
    put_number(w, fw->resident);
    put(w, " of ");
    put_number(w, (unsigned)GPU_FIRMWARE_ROLES);
    put(w, " held read-only at 0x");
    put_hex(w, (uint32_t)fw->address);
    put(w, " (");
    fmt_u32(number, fw->resident_bytes);
    put(w, number);
    put(w, " B); ");
    for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
        const struct gpu_firmware_image *image = &fw->image[i];
        if (!image->resident) { skipped++; continue; }
        held++;
        put(w, i ? "; " : "");        /* the clause for one image already uses commas; between them, a semicolon */
        put(w, gpu_firmware_role_name(i));
        put(w, ": ");
        if (!image->checksum_matches) {
            put(w, "the bytes in memory do not hash to the checksum the disk copy carried, so nothing below "
                   "is claimed about them");
            continue;
        }
        if (!image->elf64 && !image->elf32) { put(w, "held, not a container either way"); continue; }
        fmt_u32(number, image->bytes);
        put(w, image->elf64 ? "ELF64 " : "ELF32 ");
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
                put(w, image->signature_name[0] ? image->signature_name : "signature section (unnamed)");
                put(w, " ");
                fmt_u32(number, image->signature_bytes);
                put(w, number);
                put(w, " B at 0x");
                put_hex(w, image->signature_offset);
            }
        } else {
            put(w, image->fmc_template ? ", FMC header exact" : ", FMC header mismatched");
            /* "legal" and "verified" are two different findings and used to be claimed in one breath, which is
             * how a count of four mismatches ended up printed straight after a sentence saying every CRC32 had
             * been verified.  The genuine NVIDIA file carries a sh_info word per section that is not the CRC32
             * of that section's bytes - measured on fmc-570.144.bin - so the count is the container's bookkeeping
             * and not an integrity failure, and the coprocessor's own check is the signature over the hash. */
            put(w, image->fmc_sections ? ", every section's type, flags and range legal"
                                       : ", sections rejected");
            fmt_u32(number, image->crc_failures);
            put(w, ", ");
            put(w, number);
            put(w, " section(s) whose sh_info word is not the CRC32 of their bytes, reported and not obeyed: "
                   "that field is packaging bookkeeping, and what authenticates the image is the signature over "
                   "the hash, which is the coprocessor's check to make");
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
                put(w, " B - the four numbers the FSP verify command is given");
            } else
                put(w, "; not all four of image, hash, publickey and signature are present");
        }
    }
    if (held && !skipped) put(w, "; every image the boot sequence kept was parsed");
    if (skipped) {
        /* Two different shortages used to share one sentence: this clause said "more on the disk than the boot
         * allocation could keep" on a volume where the four missing images are not on the disk at all, which is
         * the opposite finding - one asks for a bigger allocation, the other asks for files.  The record keeps
         * which of the two it is, so the report says which. */
        unsigned kept_out = 0, absent = 0;
        for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
            if (fw->image[i].resident) continue;
            if (fw->image[i].bytes) kept_out++;
            else absent++;
        }
        put(w, "; ");
        if (absent) {
            fmt_u32(number, absent);
            put(w, number);
            put(w, " of the five roles this volume does not carry at all, so no allocation of any size could "
                   "have kept them");
        }
        if (kept_out) {
            put(w, absent ? ", and " : "");
            fmt_u32(number, kept_out);
            put(w, number);
            put(w, " read off the disk and then dropped for want of a page to keep them in");
        }
    }
    /* Whether these bytes reached the card is a fact about this boot, and it used to be stated as a standing
     * excuse - "the command that would consume these images is not implemented in this OS" - three builds after
     * the command was written, sent, and answered with silence.  A reader who is being told what the card saw
     * has to be told what the card saw, so the three states are named separately: held and sent, held and
     * attempted, held and not attempted.  gpu_fsp_state() is the same record the mailbox paragraph quotes two
     * lines later, so the two cannot drift apart. */
    {
        const struct gpu_fsp_state *fs = gpu_fsp_state();
        if (fs && fs->handover_sent)
            put(w, ".  These bytes did not stop at being held: the handover they describe was written to the "
                   "FSP's channel this boot, and the card's answer - reply or none - is the mailbox paragraph "
                   "below, which is where the measurement lives rather than in this sentence.\n");
        else if (fs && fs->handover_attempted)
            put(w, ".  A handover was attempted from these bytes and did not reach an answer, which is the "
                   "difference this paragraph exists to draw: the image is in memory, the packet went as far as "
                   "the card let it, and the next paragraph says how far that was.\n");
        else
            put(w, ".  Nothing was written to the card from these bytes this boot: the payload they describe was "
                   "not assembled, and the mailbox paragraph below gives the reason rather than leaving it to "
                   "be guessed.\n");
    }
}

/* The firmware paragraph of the report, kept out of graphics_report()'s body because it has two states
 * worth reading whole - nothing staged, and something staged - rather than one sentence bolted onto the
 * end of another. */
static void put_firmware_report(struct writer *w, const struct gpu_firmware_state *fw)
{
    unsigned missing = 0;

    if (!fw || !fw->staged) {
        put(w, "GSP firmware: none of the images the open driver loads to start this coprocessor is on "
               "the boot volume - absent: ");
        for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
            put(w, i ? ", " : "");
            put(w, gpu_firmware_role_name(i));
            if (fw && fw->role[i].state == BOOT_FIRMWARE_STATE_ABSENT) missing++;
        }
        put(w, ". Upstream they are nvidia/<chip>/gsp/{gsp,fmc,bootloader,booter_load,booter_unload}-"
               "<version>.bin - 570.144 for the GB207 in this machine - and on this disk they belong in the "
               "SCOS directory at the volume root as GSP.BIN, GSPFMC.BIN, GSPBL.BIN, GSPBTL.BIN and "
               "GSPBTU.BIN, which tools/makedisk.py will copy and name from an unpacked driver package. "
               "Nothing is refused for wanting them: the stub reads and checks whichever of the five it "
               "finds.\n");
        return;
    }
    put(w, "GSP firmware: ");
    put_number(w, fw->staged);
    put(w, " of ");
    put_number(w, (unsigned)GPU_FIRMWARE_ROLES);
    put(w, " image(s) staged by the boot stub, ");
    put_number(w, fw->total_bytes);
    put(w, " B read in total, set ");
    if (fw->complete)
        put(w, "complete for the boot sequence to ask for");
    else if (fw->staged == (unsigned)GPU_FIRMWARE_ROLES)
        put(w, "present and not accepted: all five are on the volume, and the reason is stated per image "
               "below");
    else
        put(w, "incomplete: this volume carries fewer of the five than the boot sequence asks for");
    if (!fw->consistent)
        put(w, " and inconsistent: the summary the stub sent disagrees with its own per-file records, so "
               "the numbers below are the ones this kernel re-added from those records");
    put(w, ". ");
    for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
        if (fw->role[i].state == BOOT_FIRMWARE_STATE_ABSENT) { missing++; continue; }
        put_firmware_role(w, fw, i);
    }
    if (missing) {
        put(w, ". Not on the disk: ");
        for (unsigned i = 0, first = 1; i < GPU_FIRMWARE_ROLES; i++) {
            if (fw->role[i].state != BOOT_FIRMWARE_STATE_ABSENT) continue;
            put(w, first ? "" : ", ");
            first = 0;
            put(w, gpu_firmware_role_name(i));
        }
    }
    put(w, "\nGSP firmware intake: every byte above was read and checked");
    put(w, fw->resident ? ", and the copies this kernel was given are still held - the paragraph below "
                         "says what is in them"
                       : ", then let go: the boot stub releases the pool it read a file into as soon as it "
                         "has examined it");
    put(w, ". This build puts an image in the coprocessor's channel as soon as that channel reports itself free, "
           "and asks its capability query afterwards rather than before, so on a machine whose mailbox never "
           "answers the question is no longer whether this OS was too careful to send anything but whether the "
           "packet went out and was ignored - the paragraph below answers it with the stores it lists, and this "
           "sentence used to promise the opposite condition, which the card has now disproved once per gate. "
           "What was checked is shape, not signature: a role reading no container means the file is neither of "
           "the two ELF images "
           "the driver names - a text file, an HTML download page, or a copy interrupted halfway - and an "
           "unreadable one means the firmware could not open it at all, which its read caps at 16 MiB. The "
           "blobs inside a signature section are verified by the hardware, which is the only party able to, "
           "and nothing here claims to have approved one.\n");
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
    /* The one question that can only be answered by trying: the queue pair is zeroed the way the reference
     * driver zeroes it on receive, and what comes back is the fact.  Before this, a card that refuses reads was
     * never written to at all, and the report could only say nobody answered without saying nobody asked. */
    put(w, " The reply queue was written 0 in both words and read back 0x");
    put_hex(w, f->reset_head);
    put(w, "/0x");
    put_hex(w, f->reset_tail);
    put(w, f->queue_writable
            ? ", so the queue takes stores from this reader and a packet can be put on it"
            : ", which refuses the store as well as the read: this reader has no door into the queue");
    put(w, ".");
    put(w, " The EMEM offset advanced ");
    put_number(w, f->send_walk);
    put(w, " dword(s) while the packet was written, which is reported and not used to veto the send: the "
           "coprocessor judges a packet by its own length and signature, and an unreadable counter is not "
           "evidence that the bytes did not arrive.");
    put(w, " The FSP's firmware version reads ");
    put_number(w, f->ucode_version & 0x7ffu);
    put(w, " and its own fuse check reads 0x");
    put_hex(w, f->fuse_check);
    put(w, ", which is what a pass looks like.  The coprocessor's capability word at BAR0+0x1100f4 ");
    if (f->gsp_readable) {
        put(w, "answered 0x");
        put_hex(w, f->hwcfg2);
        put(w, ", which is the change a successful boot would cause: it is the word every reader has so far "
               "been refused.");
    } else {
        put(w, "still answers the refusal this OS has recorded before - 0x");
        put_hex(w, f->hwcfg2);
        put(w, " - so the door the security processor holds is still shut, and this build has no reason to "
               "believe otherwise.");
    }
    put(w, "  A capability query (");
    put_number(w, f->query.request_bytes);
    put(w, " bytes: an NVDM_TYPE_CAPS_QUERY asking which clock-boost capability the FSP supports, the one "
           "request the reference driver itself sends before it has unlocked or configured anything) ");
    if (!f->responded) {
        /* One sentence per reason, because they mean four different things to whoever is reading this: the
         * queues never fell idle, they fell idle but refused while being looked at, the channel swallowed
         * the packet without walking its offset, and the FSP received a request it did not answer.  Only
         * the last of those is the card being unresponsive, and the panel has not earned saying so until
         * the other three have been ruled out. */
        if (f->send_blocked == 1) {
            put(w, "could not be started: the queue pointers were still held open by something else after ");
            put_number(w, (unsigned)f->polled);
            put(w, " looks at them, so no byte was written to the channel");
        } else if (f->send_blocked == 2) {
            put(w, "could not be started: the queue registers began refusing while this kernel was waiting "
                   "for them to fall idle, after ");
            put_number(w, (unsigned)f->polled);
            put(w, " looks");
        } else if (f->send_blocked == 3) {
            put(w, "went into the channel and the channel's offset did not advance as those writes say it "
                   "should have, so the doorbell was never rung: a half-written request is not worth a "
                   "card's time, and nothing further was sent");
        } else if (f->queried) {
            put(w, "was put in the channel and no reply came back: the reply queue's two pointers never "
                   "separated within ");
            put_number(w, (unsigned)f->reply_polls);
            put(w, " looks at them.  A refusal to answer is a result - it says the mailbox is there and the "
                   "thing behind it is not reading, which is what a boot that never reached the FSP looks "
                   "like from outside");
        } else {
            put(w, "was never started: the FSP has not reported itself booted, and a request written into a "
                   "channel nobody is reading is not a query");
        }
        if (f->query.oversize) {
            put(w, "  The FSP did put a reply of ");
            put_number(w, (unsigned)f->query.oversize);
            put(w, " bytes in the channel, longer than anything this query can have asked for, so it was left "
                   "where it is rather than read into a buffer too small for it.");
        }
        return;
    }
    put(w, "was sent, and a reply came back in ");
    put_number(w, (unsigned)f->query.bytes);
    put(w, " bytes: start ");
    put_number(w, (unsigned)f->query.som);
    put(w, ", end ");
    put_number(w, (unsigned)f->query.eom);
    put(w, " of a message, NVDM type 0x");
    put_hex(w, (uint32_t)f->query.type);
    put(w, ", payload kind 0x");
    put_hex(w, (uint32_t)f->query.kind);
    put(w, " (0x15 is NVDM_TYPE_FSP_RESPONSE, which is what an FSP reply is required to say it is), task ");
    put_number(w, f->query.task);
    put(w, " in answer to command 0x");
    put_hex(w, f->query.command);
    put(w, ", error 0x");
    put_hex(w, f->query.error);
    put(w, ".  An error of 0 is the FSP saying it understood the request; it is not the card being "
           "accelerated, and nothing on this line sets a register that could make it so.");
    /* Submission needs two things the card has to provide: a clock to measure against, and a page the host is
     * allowed to ring a doorbell through.  Both are read at the offsets the published Blackwell headers name
     * rather than at the legacy user-mode page, because NVIDIA's own source says that page is going away from
     * Turing onwards and the doorbell it used to hold now lives in the virtual-function page - so a silent
     * 0x810000, which is what an earlier build of this OS reported, would have been a fact about a page and
     * not about this card.  Nothing here is written: the doorbell at 0x2200 is documented write-only, and a
     * doorbell rung for a channel that does not exist is one way to hang a card. */
    put(w, "  A submission also needs a clock and a doorbell, and both were looked for where this generation "
           "documents them: the timer block's clock at BAR0+0x9400 and the virtual-function page at "
           "BAR0+0x0..0x2ffff, whose doorbell at +0x2200 is write-only and was left alone.  The timer ");
    if (!f->timer_readable) put(w, "block's window could not be mapped, so this OS makes no claim about the clock");
    else if (f->timer_refused) put(w, "is refused in the same family of codes the coprocessor's block is, so "
                                       "nothing is claimed about whether a clock is running behind it");
    else if (f->timer_dead) put(w, "answers all-ones at both halves, which is no reply at all: the block is "
                                   "not decoded or not powered, and until it answers there is nothing to time a "
                                   "submission against");
    else {
        put(w, "read 0x");
        put_hex(w, f->timer_start);
        put(w, " and then 0x");
        put_hex(w, f->timer_end);
        put(w, f->timer_ticking ? ", which is the clock advancing: the GPU's own time is running and readable to "
                                  "this OS with no setup at all"
                                : ", the same value twice, so the block answers and its counter does not move - "
                                  "which is what an idle clock-gated block looks like from the host, and the "
                                  "reason nothing was submitted through a doorbell");
    }
    put(w, ".  Of the six words read in the virtual-function page, ");
    if (!f->vf_readable) put(w, "none were reached, because this kernel could not map that page - a fact about "
                                "this kernel's mapping, not about the chip");
    else {
        put_number(w, (unsigned)f->vf_answered);
        put(w, " answered with content, ");
        put_number(w, (unsigned)f->vf_refusals);
        put(w, " came back as a PRI refusal and the rest read zero or all-ones; the interrupt tree's first leaf "
               "is 0x");
        put_hex(w, f->vf_leaf0);
        put(w, ", its first top node 0x");
        put_hex(w, f->vf_top0);
        put(w, ", and the four mailbox scratch words the coprocessor's host interface uses are 0x");
        for (unsigned i = 0; i < 4u; i++) {
            if (i) put(w, "/0x");
            put_hex(w, f->vf_scratch[i]);
        }
        put(w, f->vf_answered ? ", which is a page that is answering, whatever it has to say"
                              : ", which is a page that is there and empty");
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
    put(w, "FSP mailbox: ");
    if (!f) {
        put(w, "nothing was asked.  This boot has no display function for the question to be put to - the "
               "mailbox is read out of a card's own BAR0, so a machine with no PCI display function has "
               "nothing for it to be at.\n");
        return;
    }
    if (!f->nvidia) {
        put(w, "not asked, and not because anything failed.  Every offset on this path is where an NVIDIA "
               "chip decodes it inside an NVIDIA function's BAR0, and this machine's display function is "
               "vendor 0x");
        put_hex(w, f->vendor);
        put(w, ", whose aperture holds different things at the same addresses.  Reading them would be a "
               "guess about somebody else's hardware and writing them would not be a guess at all, so no "
               "byte of this function's BAR was read, and none was written.\n");
        return;
    }
    if (!f->attempted) {
        put(w, "the NVIDIA function at BAR0 0x");
        put_hex(w, (uint32_t)f->bar0);
        put(w, f->skipped == 2 ? " sits above the range this kernel maps devices into, so its mailbox page at "
                                 "+0x8f0000 was never reached and nothing was asked"
                               : " has no memory aperture at all, which means its memory decoding is off or "
                                 "the firmware never gave it an address, so there was nothing to ask through");
        put(w, ".\n");
        return;
    }
    if (!f->mapped) {
        put(w, "the windows this path asks for - BAR0+0x20000 for the boot scratch, +0x8f0000 for the mailbox, "
               "+0x110000 for the coprocessor's falcon, +0x0 for the virtual-function page and +0x9000 for the "
               "timer block - could not be mapped, so the registers were never read and nothing was written.\n");
        return;
    }
    put(w, "asked of the card at BAR0 0x");
    put_hex(w, (uint32_t)f->bar0);
    put(w, ", through five windows of its own aperture, on a boot that wrote ");
    put_number(w, (unsigned)f->writes);
    put(w, " register(s) and read ");
    put_number(w, (unsigned)f->reads);
    put(w, " of them (");
    put_number(w, f->refusal_reads);
    put(w, " of the reads were the 0xbadf4100 refusal that says the coprocessor's block is locked until the "
           "FSP releases it).  ");
    /* What the handover will be measured against, printed ahead of the outcome: on this generation the three
     * blob sizes are the difference between a firmware file being accepted and being refused as malformed,
     * and the row of the table that applies is the chip's architecture, not this file's preference. */
    put(w, "The architecture field of NV_PMC_BOOT_0 reads 0x");
    put_hex(w, f->arch);
    if (f->arch_unknown)
        put(w, " and would not answer - 0, all ones, or a PRI error - so the size this chip's FSP wants for the "
               "three authentication blobs is unknown and nothing is sent: a container is not judged against "
               "an expectation invented for it");
    else if (!f->fsp_supported)
        put(w, ", a generation the reference driver has no FSP entry for: no mailbox exists at these offsets "
               "on it, and its coprocessor is reached through the older ACR path, so nothing here is asked of "
               "it");
    else {
        put(w, ", whose FSP wants a hash of ");
        put_number(w, f->cot_hash_bytes);
        put(w, ", a public key of ");
        put_number(w, f->cot_pkey_bytes);
        put(w, " and a signature of ");
        put_number(w, f->cot_sig_bytes);
        put(w, " bytes, in a container of version ");
        put_number(w, f->cot_version);
        put(w, ".  Hopper's table says 48/384/384 and both Blackwell tables say 48/97/96, which is why one "
               "hard-coded size in this file used to be enough and now is not");
    }
    put(w, ".  ");
    put(w, "The FSP's boot-complete scratch at BAR0+0x200bc reads 0x");
    put_hex(w, f->boot_scratch);
    put(w, f->booted ? ", which is the value that means it has finished booting, so the mailbox behind it is "
                      "worth asking"
                     : ", which is not 0xFF: the FSP has not reported itself booted, and until it does "
                       "nothing is reading the channel");
    if (!f->booted) {
        /* Counted, not described: the only honest way to say "it did not answer in time" is to say how much
         * time that was.  The budget is the reference driver's own four seconds, and it is measured against
         * the coprocessor's PTIMER - the cycle counter timed against that clock once per probe - because a
         * bound expressed as a count of uncached reads is a claim about this CPU, not about the chip.  When
         * there is no clock to read, the panel says the bound was a count instead of pretending otherwise. */
        put(w, ".  That was waited for: ");
        if (f->wait_clocked) {
            put_number(w, (unsigned)f->boot_ms_used);
            put(w, " ms of the ");
            put_number(w, (unsigned)f->boot_ms);
            put(w, " ms the reference driver allows, in ");
            put_number(w, (unsigned)f->boot_polls);
            put(w, " reads at 0x200bc, and then it stopped rather than holding a boot open.  That is the whole "
                   "budget, not a fraction of it: a card that has not reported itself booted after four seconds "
                   "of its own clock is not a card that was still on its way");
        } else {
            put_number(w, (unsigned)f->boot_polls);
            put(w, " reads at 0x200bc.  The coprocessor's timer block would not answer, so this wait could not "
                   "be promised in milliseconds and was bounded by a count of reads instead - which says how "
                   "much work was done, and not how much time passed, and the difference is printed rather than "
                   "hidden");
        }
    }
    if (!f->fsp_supported) {
        /* Said and stopped, rather than tried and reported as a failure: the paragraphs below would each
         * describe an exchange this OS deliberately did not start. */
        put(w, ".\nGSP handover: not staged, and nothing was asked of the mailbox, because the page an FSP "
               "queue would live on is not a queue on this generation.  The reference driver reaches this "
               "chip's coprocessor through the older ACR path, which is not implemented here, so the honest "
               "report is that this OS has no business writing to these offsets on this card.\n");
        return;
    }
    if (!f->mailbox_readable) {
        put(w, ".  The mailbox queue registers themselves refused, so no request was attempted: the "
               "capability query below was never started, and that is the honest order of the two facts.\n");
        return;
    }
    put_fsp_mailbox(w, f);
    put(w, "\n");
    put(w, "GSP handover: ");
    if (f->handover_sent) {
        /* The message left this machine, so the paragraph is about what came back and what the two words that
         * a boot would change now read.  Saying them as a pair, before and after, is the whole point: one
         * number alone could have been a measurement of anything, and a change in a locked register is the
         * only evidence this OS can produce that the card did something. */
        put(w, "the ");
        put_number(w, GPU_FSP_PAYLOAD_BYTES);
        put(w, "-byte payload was written into the channel and the doorbell rung.  It carried the FMC image of ");
        put_number(w, (unsigned)f->image_bytes);
        put(w, " bytes at physical 0x");
        if (f->image_physical >> 32) put_hex(w, (uint32_t)(f->image_physical >> 32));
        put_hex(w, (uint32_t)f->image_physical);
        put(w, ", CRC32 0x");
        put_hex(w, (uint32_t)f->image_crc32);
        if (!f->handover.bytes) {
            put(w, ", and no answer came back: the reply queue's pointers never separated within ");
            put_number(w, (unsigned)f->handover_polls);
            put(w, " looks at them.  That is the answer a card gives when it read a request it was not "
                   "configured to serve, and it is reported instead of retried - a second handover into a "
                   "channel the first one is possibly still being read would tell nothing about the first.");
        } else {
            put(w, "; the FSP answered in ");
            put_number(w, (unsigned)f->handover.bytes);
            put(w, " bytes with error 0x");
            put_hex(w, f->handover.error);
            {
                const char *name = gpu_fsp_flcn_name(f->handover.error);
                if (name) { put(w, " ("); put(w, name); put(w, ")"); }
                else put(w, ", a code this build does not have a name for");
            }
            put(w, ", and the coprocessor's capability word read 0x");
            put_hex(w, f->hwcfg2);
            put(w, " before the message and 0x");
            put_hex(w, f->hwcfg2_after);
            put(w, f->gsp_released ? " after it: the block that refused every read on this panel is answering, "
                                     "which is the change the reference driver's own release check waits for"
                                   : " after it, still the locked-target refusal - the FSP took the message "
                                     "without opening the coprocessor's block to a cold reader, which is what "
                                     "the protected region and the resource manager's image exist to change");
            put(w, ".  Bit 13 of that word - NV_PFALCON_FALCON_HWCFG2_RISCV_BR_PRIV_LOCKDOWN, which is the "
                   "lock itself rather than a read that looked friendly - was ");
            put(w, f->lockdown < 0 ? "unreadable before the message"
                 : f->lockdown ? "set (locked)" : "clear (unlocked)");
            put(w, " and ");
            put(w, f->lockdown_after < 0 ? "unreadable after it"
                 : f->lockdown_after ? "set (locked)" : "clear (unlocked)");
            put(w, ".  The falcon's own mailbox pair read 0x");
            put_hex(w, f->gsp_mailbox0);
            put(w, "/0x");
            put_hex(w, f->gsp_mailbox1);
            put(w, " before and 0x");
            put_hex(w, f->gsp_mailbox0_after);
            put(w, "/0x");
            put_hex(w, f->gsp_mailbox1_after);
            put(w, " after: zero means the FMC has not reported yet, and a non-zero pair is its boot-arguments "
                   "address or an error code, which this OS cannot tell apart because it hands over no "
                   "arguments for the FMC to write back - the reference driver compares that pair against the "
                   "address of a GSP_FMC_BOOT_PARAMS block it allocated, and this kernel has no resource "
                   "manager to point at");
            put(w, ".  The FSP's boot-complete scratch read 0x");
            put_hex(w, f->boot_scratch);
            put(w, " before and 0x");
            put_hex(w, f->boot_scratch_after);
            put(w, " after, and the channel's queue pointers ");
            put(w, f->queues_idle_after ? "fell idle again afterwards, which is what they must do before a "
                                          "second message can be sent"
                                        : "were still held open when this was measured, so the exchange had "
                                          "not finished by then");
            put(w, ".\n");
        }
        return;
    }
    if (f->handover_attempted) {
        put(w, "the ");
        put_number(w, GPU_FSP_PAYLOAD_BYTES);
        put(w, "-byte payload went to the channel and the write was refused before the doorbell could be rung");
        if (f->handover_blocked == 1) {
            put(w, ": the queue pointers were still held open by something else.  The count of looks that "
                   "waited is the one quoted for the query above, because it is one counter for the channel "
                   "and printing it twice would make it look like a measurement of this message");
        } else if (f->handover_blocked == 2) {
            put(w, ": the queue registers began refusing while this kernel waited for them to fall idle");
        } else {
            put(w, ": the channel's offset did not advance as the writes say it should have, so nothing was "
                   "claimed to have been sent");
        }
        put(w, ".  The staged image stays where it is and nothing was re-tried.\n");
        return;
    }
    if (f->staged && f->cot_ready) {
        put(w, "one NVDM_PAYLOAD_COT of ");
        put_number(w, GPU_FSP_PAYLOAD_BYTES);
        put(w, " bytes was assembled from the FMC image the boot volume offered and is sitting in memory this "
               "kernel owns - the image is ");
        put_number(w, (unsigned)f->image_bytes);
        put(w, " bytes copied page-aligned and zero-padded to physical 0x");
        if (f->image_physical >> 32) { put_hex(w, (uint32_t)(f->image_physical >> 32)); }
        put_hex(w, (uint32_t)f->image_physical);
        put(w, ", which is the address the payload carries, and its CRC32 over the copy the card would read is "
               "0x");
        put_hex(w, (uint32_t)f->image_crc32);
        put(w, " (the container's own checksum covers the file, not the copy).  Hash ");
        put_number(w, f->cot_hash_bytes);
        put(w, " bytes, public key ");
        put_number(w, f->cot_pkey_bytes);
        put(w, " bytes and signature ");
        put_number(w, f->cot_sig_bytes);
        put(w, " bytes were taken out of the FMC's sections and placed where the published payload puts them - "
               "the widths are the ones this architecture's own table gives, quoted here rather than remembered, "
               "because Hopper's is 48/384/384 and Blackwell's is not, and a report that recites the older line "
               "describes a card that is not in the machine.  The resume store is described as absent, which is "
               "how the reference driver encodes a machine that has nothing to resume.  It went on the wire "
               "first and without conditions, because that is what the driver that boots this chip does: no "
               "capability query, no permission, one packet and the doorbell.  ");
        if (f->handover_sent)
            put(w, "The card answered it, and the answer is in the numbers below.");
        else if (f->handover_attempted)
            put(w, "The card did not answer the handover (it was written, and the reply queue's two pointers "
                    "never separated); the payload also names no FRTS region and no boot arguments, which is "
                    "what this OS can honestly offer today ");
        else
            /* The one case left after the gate came out: a queue that says it is busy.  The reference driver
             * waits on that same pair of pointers before it touches QUEUE_TAIL, so this is the condition the
             * file still declines on, and it is named as itself rather than as a policy about firmware. */
            put(w, "It was never written: the queue did not report itself free at the moment of sending, and "
                   "this driver does not submit into a queue that says it is busy.");
        put(w, "\n");
        /* Which stores actually reached the card, as opposed to how many: the total has twice been the whole
         * argument in a debugging round, and a total cannot distinguish a doorbell rung from a payload written
         * and the queue left alone. */
        {
            unsigned n = f->store_count < GPU_FSP_STORE_LOG ? (unsigned)f->store_count : GPU_FSP_STORE_LOG;
            unsigned first = (unsigned)(f->store_count - n);
            put(w, " The last ");
            put_number(w, n);
            put(w, " store(s) this driver issued, oldest first, as BAR0 offset and value:");
            for (unsigned i = 0; i < n; i++) {
                const struct gpu_fsp_store *st = &f->stores[(first + i) % GPU_FSP_STORE_LOG];
                put(w, i ? ", 0x" : " 0x");
                put_hex(w, st->offset);
                put(w, "=0x");
                put_hex(w, st->value);
            }
            if (f->store_count > n) {
                put(w, ", of ");
                put_number(w, (unsigned)f->store_count);
                put(w, " issued in all");
            }
            put(w, ".  The channel is 0x8f2ac0 and its window 0x8f2ac4, the queue tail - the doorbell - is "
                    "0x8f2c04 and the head 0x8f2c00: a tail in this list whose value is one dword short of the "
                    "packet's length is a card that was asked, whatever it said, and a list that ends at a "
                    "payload dword is a driver that stopped before asking.  Which of the two happened is no "
                    "longer something this report has to be argued about.\n");
        }
    } else if (f->fmc_seen) {
        put(w, "the FMC image on the boot volume was read, but it cannot be described in this payload: ");
        switch (f->cot_defect) {
            case 1: put(w, "its hash section is not 48 bytes, and the published payload has 12 words for a "
                            "SHA-384 and no room for any other size"); break;
            case 2: put(w, "its public key section is not the ");
                    put_number(w, f->cot_pkey_bytes);
                    put(w, " bytes this architecture's FSP table asks for, and the published payload has no room "
                           "for any other size of"); break;
            case 3: put(w, "its signature section is not the ");
                    put_number(w, f->cot_sig_bytes);
                    put(w, " bytes this architecture's FSP table asks for, and the same limit applies to it");
                    break;
            case 4: put(w, "its image section is empty or larger than the 8 MiB this kernel will copy"); break;
            case 5: put(w, "the container's sections are not all individually verifiable, so there is no "
                            "authenticated image to point the FSP at"); break;
            case 6: put(w, "this kernel could not get page-aligned memory below 46 bits for the image to be "
                            "copied into, and an offset handed to a microcontroller has to be one nothing "
                            "else can move while it is being read"); break;
            case 7: put(w, "the bytes in the boot allocation no longer match the checksum the stub recorded "
                            "for them, so what this kernel would hand over is not the file that was on the "
                            "disk"); break;
            case 8: put(w, "the container does not hold all four of image, hash, publickey and signature, "
                            "which is exactly what a handover payload has fields for"); break;
            default: put(w, "reason code "); put_number(w, (unsigned)f->cot_defect); break;
        }
        put(w, ", and no address or blob was staged.  Saying which of these it was is the point: a file of the "
               "wrong version and a file that got truncated on the way to the disk look the same from the "
               "outside.\n");
    } else {
        put(w, "nothing to assemble it from: no FMC image was staged on the boot volume and parsed, so the "
               "payload's image offset, hash, public key and signature would all have to be invented, and an "
               "invented authentication blob is not a thing this OS will hand a security processor.  The "
               "paragraph above names the five files the boot volume can be given.\n");
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
        put(&w, "CPU software compositor, GPU engine painting solid output rectangles\n");
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
            ? "used for solid output rectangles"
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
                    put(&w, " reads this chip itself and quotes what it found above, so the registers were "
                             "read; they were just not read by the code that draws this panel");
                } else
                    put(&w, " (a driver module is staged for this function, but it is not resident, so "
                            "nobody has read it)");
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
    put(&w, "Detection is not driver support: no BAR sizing, GPU reset or modeset was "
            "performed (config writes during detection: ");
    put_number(&w, (unsigned)gpu_config_writes_during_detect());
    put(&w, ").\n");
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
        const struct gpu_fsp_state *fs = gpu_fsp_state();
        if (gsp && gsp->bound) {
            put(&w, "GSP path: the clause above quotes what the chip answered to reads alone, and the "
                    "paragraph before this one is the only thing this OS has ever written to a card to make "
                    "it answer");
            if (fs && fs->handover_sent)
                put(&w, ", including one handover of a firmware image the boot volume supplied - what the card "
                        "made of it is in the two measurements taken around it, not in this sentence");
            else
                put(&w, " - one query, no boot");
            put(&w, ".  Beyond that sits the card's own firmware boot: this build reads the firmware off the "
                    "boot volume if it is given it, assembles the handover, and asks the FSP to use it, and an "
                    "engine whose coprocessor was not started for it is an engine this build cannot give work "
                    "to yet.\n");
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
