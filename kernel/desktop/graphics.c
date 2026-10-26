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
#define REPORT_LIST_LIMIT 4

static struct boot_framebuffer scanout;

void graphics_init(const struct boot_framebuffer *fb)
{
    scanout = *fb;
    gpu_init(fb);
}

struct writer {
    char *at;
    size_t left;
};

static void put(struct writer *w, const char *text)
{
    while (*text && w->left > 1) { *w->at++ = *text++; w->left--; }
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
            put(w, image->fmc_sections ? ", every section legal and its CRC32 verified"
                                       : ", sections rejected");
            fmt_u32(number, image->crc_failures);
            put(w, ", ");
            put(w, number);
            put(w, " CRC32 mismatch(es)");
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
        put(w, "; ");
        fmt_u32(number, skipped);
        put(w, number);
        put(w, " more on the disk than the boot allocation could keep");
    }
    put(w, ". Nothing was written to the card: the command that would consume these images is not implemented "
           "in this OS, so what this line describes is memory SCos is holding, not a coprocessor that was "
           "told anything.\n");
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
    put(w, ". This build has no code that hands an image to the coprocessor, so staging firmware changes what "
           "the OS can tell you about the disk and not yet what it can do to the card. What was checked is "
           "shape, not signature: a role reading no container means the file is neither of the two ELF images "
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
    put_number(w, f->packet_bytes);
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
        if (f->reply_oversize) {
            put(w, "  The FSP did put a reply of ");
            put_number(w, (unsigned)f->reply_oversize);
            put(w, " bytes in the channel, longer than anything this query can have asked for, so it was left "
                   "where it is rather than read into a buffer too small for it.");
        }
        return;
    }
    put(w, "was sent, and a reply came back in ");
    put_number(w, (unsigned)f->reply_bytes);
    put(w, " bytes: start ");
    put_number(w, (unsigned)f->reply_som);
    put(w, ", end ");
    put_number(w, (unsigned)f->reply_eom);
    put(w, " of a message, NVDM type 0x");
    put_hex(w, (uint32_t)f->reply_type);
    put(w, ", payload kind 0x");
    put_hex(w, (uint32_t)f->reply_kind);
    put(w, " (0x15 is NVDM_TYPE_FSP_RESPONSE, which is what an FSP reply is required to say it is), task ");
    put_number(w, f->reply_task);
    put(w, " in answer to command 0x");
    put_hex(w, f->reply_command);
    put(w, ", error 0x");
    put_hex(w, f->reply_error);
    put(w, ".  An error of 0 is the FSP saying it understood the request; it is not the card being "
           "accelerated, and nothing on this line sets a register that could make it so.");
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
        put(w, "the three one-page windows this path needs - BAR0+0x20000 for the boot scratch, +0x8f0000 for "
               "the mailbox and +0x110000 for the coprocessor's falcon - could not be mapped, so the "
               "registers were never read and nothing was written.\n");
        return;
    }
    put(w, "asked of the card at BAR0 0x");
    put_hex(w, (uint32_t)f->bar0);
    put(w, ", through three one-page windows of its own aperture, on a boot that wrote ");
    put_number(w, (unsigned)f->writes);
    put(w, " register(s) and read ");
    put_number(w, (unsigned)f->reads);
    put(w, " of them (");
    put_number(w, f->refusal_reads);
    put(w, " of the reads were the 0xbadf4100 refusal that says the coprocessor's block is locked until the "
           "FSP releases it).  ");
    put(w, "The FSP's boot-complete scratch at BAR0+0x200bc reads 0x");
    put_hex(w, f->boot_scratch);
    put(w, f->booted ? ", which is the value that means it has finished booting, so the mailbox behind it is "
                      "worth asking"
                     : ", which is not 0xFF: the FSP has not reported itself booted, and until it does "
                       "nothing is reading the channel.  The reference driver waits up to four seconds for "
                       "this after resetting a chip; this OS does not reset one, so on a machine where the "
                       "firmware on the card never got this far the wait would end the same way"
       );
    if (!f->mailbox_readable) {
        put(w, ".  The mailbox queue registers themselves refused, so no request was attempted: the "
               "capability query below was never started, and that is the honest order of the two facts.\n");
        return;
    }
    put_fsp_mailbox(w, f);
    put(w, "\n");
    put(w, "GSP handover: ");
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
        put(w, " (the container's own checksum covers the file, not the copy).  Hash 48 bytes, public key 384, "
               "signature 384 were taken out of the FMC's sections and placed where the published payload puts "
               "them, and the resume store is described as absent, which is how the reference driver encodes a "
               "machine that has nothing to resume.  It is not sent: asking the FSP to boot the coprocessor also "
               "needs the write-protected region and the resource manager's own image in place, and half a boot "
               "is a machine that comes back to a black screen with no log, so the payload is built, staged and "
               "reported here while the command that would consume it stays unimplemented.\n");
    } else if (f->fmc_seen) {
        put(w, "the FMC image on the boot volume was read, but it cannot be described in this payload: ");
        switch (f->cot_defect) {
            case 1: put(w, "its hash section is not 48 bytes, and the published payload has 12 words for a "
                            "SHA-384 and no room for any other size"); break;
            case 2: put(w, "its public key section is not 384 bytes, which the published payload has no room "
                            "for any other size of"); break;
            case 3: put(w, "its signature section is not 384 bytes, and the same limit applies to it"); break;
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

void graphics_report(char *out, size_t capacity)
{
    struct writer w = {out, capacity};
    char number[24];
    const struct gpu_driver *engine = gpu_bound_driver();
    const struct gpu_device *owner = gpu_scanout_device();

    if (!out || capacity == 0) return;
    *out = 0;                     /* every write below goes through put(), which bounds */

    /* Three states, described differently on purpose: an engine that paints the console, an engine
     * that works but sits on a function that does not feed the console, and no engine at all.  The
     * middle one is the state a virtual machine is usually in, and a report that called it either
     * "accelerated" or "unavailable" would be lying. */
    const struct gpu_module_state *ms = gpu_module_state();
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
        char detail[DETAIL_LIMIT];
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
        put_firmware_report(&w, gpu_firmware_state());
        put_firmware_image(&w, gpu_firmware_state());
        put_fsp_report(&w, gpu_fsp_state());
        const struct gpu_module_state *gsp = gpu_module_state();
        if (gsp && gsp->bound) {
            put(&w, "GSP path: the clause above quotes what the chip answered to reads alone, and the "
                    "paragraph before this one is the only thing this OS has ever written to a card to make "
                    "it answer - one query, not a boot.  What would start the coprocessor this OS neither "
                    "carries nor sends: it reads the firmware off the boot volume if it is given it, assembles "
                    "the handover, and stops there, so an engine whose coprocessor was not started for it is "
                    "an engine this build cannot give work to.\n");
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
}
