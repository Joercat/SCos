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
    if (!(r->flags & BOOT_FIRMWARE_ELF64)) {
        put(w, ", no container");
        if (r->flags & BOOT_FIRMWARE_TRUNCATED) put(w, ", truncated");
        return;
    }
    put(w, ", ELF64 ");
    fmt_u32(number, r->sections);
    put(w, number);
    put(w, "s");
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
    put(w, fw->complete ? "complete for the boot sequence to ask for" : "incomplete");
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
    put(w, "\nGSP firmware intake: every byte above was read, checked, and let go - this build has no code "
           "that hands an image to the coprocessor, so staging firmware changes what the OS can tell you "
           "about the disk and not yet what it can do to the card. What was checked is shape, not "
           "signature: a role reading no container means the file is not the ELF64 image the boot sequence "
           "takes .fwimage out of (a text file, an HTML download page, or a copy interrupted halfway), and an "
           "unreadable one means the firmware could not open it at all, which its read caps at 16 MiB. The "
           "blobs inside the signature section are verified by the hardware, which is the only party able to, "
           "and nothing here claims to have approved one.\n");
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
        const struct gpu_module_state *gsp = gpu_module_state();
        if (gsp && gsp->bound) {
            put(&w, "GSP path: the clause above quotes what the chip answered to reads alone; nothing was "
                    "written to make it answer. Where it answered an error code instead of a register, the "
                    "door is held by the card's own firmware boot, which this OS neither carries nor "
                    "speaks to - so an engine whose coprocessor was not started for it is an engine this "
                    "build cannot give work to.\n");
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
