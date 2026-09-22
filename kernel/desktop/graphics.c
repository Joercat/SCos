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
        put(&w, ") loaded from storage and verified by device readback, ");
        fmt_u32(number,(u32)ms->self_test_matches);put(&w,number);
        put(&w, "/");
        fmt_u32(number,(u32)ms->self_test_pixels);put(&w,number);
        put(&w, " pixels; ");
        put(&w, gpu_engine_drives_output()
            ? "used for solid output rectangles"
            : "not used for output: another PCI function feeds this display");
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
            put(&w, "not consulted: no PCI display function matched a driver family\n");
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
