/* GPU auto-detection: name the display adapter, then decide what SCos may do.
 *
 * The algorithm is deliberately the same shape as the upstream drivers' own
 * enumeration loops, because that is what "exact device matching" means: for a
 * function in class 0x03, require the family's vendor, its class predicate where it
 * has one, and a device ID present in that family's table.  A family with no ID table
 * (VESA, the firmware framebuffer, virtio) cannot be matched by ID at all and is only
 * ever reported as the fallback path.
 *
 * Nothing in this file writes to a device.  Identification must be safe to run on the
 * adapter that is currently producing the only visible console, so BARs are read but
 * never sized, no reset or power transition is issued, and the display remains under
 * firmware control until a family's engine code is ported and bound (see
 * `gpu_ports.c`).  Every log line states the consequence, not just the match, so a
 * boot log is readable with no keyboard attached.
 */
#include "scos.h"
#include "gpu.h"

#define SCAN_SUBLASSES 4
#define REPORT_DEVICE_LIMIT 4

static struct gpu_device devices[GPU_MAX_DEVICES];
static int device_count;
static int scanout_index = -1;
static int config_writes_before, config_writes_after;
static const struct gpu_driver *bound;
static struct boot_framebuffer output;
static int families_without_port_record;
static int named_only_devices;    /* named by the registry, bound by no driver table */

static void hex4(char *out, uint16_t value)
{
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 4; i++) out[i] = digits[(value >> (12 - i * 4)) & 15];
    out[4] = 0;
}

const char *gpu_vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1002: return "ATI/AMD";
    case 0x10de: return "NVIDIA";
    case 0x102b: return "Matrox";
    case 0x1106: return "VIA";
    case 0x10c8: return "NeoMagic";
    case 0x5333: return "S3";
    case 0x121a: return "3dfx";
    case 0x100c: return "Tseng";
    case 0x1048: return "ELSA (NVIDIA)";
    case 0x12d2: return "STB/SGS-Thompson (NVIDIA)";
    case 0x1888: return "Varisys (NVIDIA)";
    case 0x1af4: return "Virtio";
    case 0x1234: return "QEMU";
    case 0x15ad: return "VMware";
    default: return "Unknown vendor";
    }
}

/* Reading a device register needs no write, no sizing and no modeset: map the first page of BAR0 and
 * load two dwords.  On an NVIDIA function that is NV_PMC_BOOT_0 and NV_PMC_BOOT_4; NVIDIA's published
 * register documentation puts ARCHITECTURE in [28:24], IMPLEMENTATION in [23:20], MAJOR_REVISION in
 * [7:4] and MINOR_REVISION in [3:0], and the pair ARCHITECTURE:IMPLEMENTATION in [31:20] is what the
 * chip is named by (GA102 answers 0x168000a1: selector 0x168, revision A.1).  On an ATI Rage function
 * it is CONFIG_CHIP_ID and CONFIG_AMPUS.  Either way the
 * value comes out of the silicon, which is the point: it turns "we named your chip from a table" into
 * "we read your chip", and it shows whether an aperture the kernel mapped above 4 GiB is reachable on
 * real firmware.  Functions a module could claim are left alone, so an aperture is never mapped twice. */
void *device_map(uint64_t physical, uint64_t bytes, int write_combine, uint64_t *mapped_bytes);

static int probe_shadows_scanout(const struct gpu_device *g);
static int identification_only_module_bound(void);

/* Whether the firmware did anything at all about a driver module for this machine.  Standing aside is
 * right when the store staged one (or tried to and failed in a way the report already names), because the
 * design is one module per boot and an aperture must not be mapped twice.  It was wrong in the case that
 * motivated this comment: the stub's scan reached no display function, so `module_state' stayed 0, the
 * kernel waited for a driver that was never going to arrive, and the chip was read by nobody.  When the
 * store consulted nothing, the kernel reads the boot register itself and says that it did. */
static int module_store_engaged(const struct boot_handoff *handoff)
{
    return handoff && handoff->module_state != 0;
}

static void probe_registers(struct gpu_device *g, const struct boot_handoff *handoff)
{
    volatile uint32_t *mmio;
    uint64_t mapped = 0;
    void *window;

    g->reg_state = 0;
    g->reg_base = 0;
    g->reg_first = g->reg_second = 0;
    if (!g->bar[0] || g->bar[0] >> 46) return;         /* no aperture, or beyond what can be mapped */
    if (probe_shadows_scanout(g)) { g->reg_state = 5; return; }
    if (gpu_module_eligible(g)) {
        /* Standing aside for a driver is correct whenever the store is doing something about this family -
         * one module per boot is the design, and mapping the same aperture twice to ask the same question
         * twice would be worse than not asking.  The case that used to read nothing at all is an eligible
         * function on a machine where the store engaged with nothing: the boot stub never reached the bus
         * the chip sits on, so no driver was staged, and detection waited for one that was never going to
         * arrive.  The fallback is deliberately narrow - the function already being scanned out from,
         * because a report that cannot describe the device driving the screen is the worst place to be
         * silent - and it says out loud that the kernel, not a driver, did the reading. */
        if (!g->is_scanout || module_store_engaged(handoff)) { g->reg_state = 0; return; }
        g->reg_kernel_fallback = 1;
    }
    if (!(g->command & 0x2)) { g->reg_state = 4; return; }
    window = device_map(g->bar[0], 4096, 0, &mapped);
    if (!window || mapped < 8) { g->reg_state = 3; g->reg_base = g->bar[0]; return; }
    g->reg_base = g->bar[0];
    mmio = (volatile uint32_t *)window;
    g->reg_first = mmio[0];
    g->reg_second = mmio[1];
    g->reg_state = (g->reg_first == 0xffffffffu && g->reg_second == 0xffffffffu) ? 2 : 1;
}

/* Raw BAR register values only.  Sizing a BAR means writing all-ones into it first,
 * and that is precisely the kind of operation that can take away the running console,
 * so this subsystem never does it; a ported driver that needs the size asks for it in
 * its own `prepare()`, once it has a restoration path to fall back on. */
static void read_bars(struct gpu_device *g)
{
    for (int i = 0; i < 6; i++) {
        uint32_t low = pci_read32(g->bus, g->slot, g->function, (uint8_t)(0x10 + i * 4));
        g->bar[i] = low & ~0xfu;
        if ((low & 6) == 4 && i < 5) {
            uint32_t high = pci_read32(g->bus, g->slot, g->function,
                                       (uint8_t)(0x14 + i * 4));
            g->bar[i] |= ((uint64_t)high << 32);
            i++;                            /* consumed as the upper half */
        }
    }
}

/* Whether BAR0 names the same physical start the firmware is already scanning out from.  The kernel
 * maps that aperture itself, for the console, so mapping it a second time to read two dwords would be
 * redundant at best and a page-table conflict at worst. */
static int probe_shadows_scanout(const struct gpu_device *g)
{
    return g->bar[0] && output.base && (g->bar[0] & ~0xfffull) == output.base;
}

static void record(struct gpu_device *g, uint8_t bus, uint8_t slot, uint8_t function,
                   uint8_t subclass)
{
    uint32_t vendev = pci_read32(bus, slot, function, 0);
    uint32_t ccrev = pci_read32(bus, slot, function, 8);
    *g = (struct gpu_device){
        .bus = bus, .slot = slot, .function = function, .subclass = subclass,
        .vendor = (uint16_t)vendev, .device = (uint16_t)(vendev >> 16),
        .revision = (uint8_t)(ccrev >> 8),
    };
    g->command = pci_read32(bus, slot, function, 4);
    g->header_type = (pci_read32(bus, slot, function, 0xc) >> 16) & 0x7f;
    read_bars(g);
}

/* Whether this function may have a driver module read for it at all.  The naming table knows chips; a
 * module is only ever loaded for a chip that some driver's own binding table claims, and this is the
 * one place that rule lives, so the boot loop, the test harness and any later caller cannot drift. */
int gpu_module_eligible(const struct gpu_device *g)
{
    return g && g->match && !g->bound && !g->named_only;
}

static void identify(struct gpu_device *g)
{
    const struct gpu_pci_id *row = 0;
    const struct gpu_match *match = gpu_match_device(g->vendor, g->device, g->subclass, &row);
    if (!match) return;
    g->match = match;
    g->chip = row ? row->name : 0;
    if (gpu_match_row_is_registry(match, row)) {
        /* The chip is named by the registry snapshot, which knows what a device id *is* and nothing
         * about what could drive it.  Returning before the port lookup below is what stops a family
         * that has a ported engine for one generation from claiming a chip two decades newer, or from
         * having a module loaded for it by the code in gpu_module.c. */
        g->named_only = 1;
        named_only_devices++;
        return;
    }
    const struct gpu_driver *port = gpu_port_for(match->family);
    if (!port) {
        families_without_port_record++;
        klog("gpu: family %s has no port record; reporting as unbound", match->family);
        return;
    }
    if (port->state != GPU_PORT_NONE && port->ops) {
        g->bound = 1;
        bound = port;
    }
}

void gpu_init(const struct boot_framebuffer *fb)
{
    const struct boot_handoff *handoff = kernel_boot_handoff();
    int module_bound = 0;
    static const uint8_t subclasses[SCAN_SUBLASSES] = {0, 1, 2, 0x80};
    uint8_t bus[8], slot[8], function[8];

    output = *fb;
    config_writes_before = (int)pci_config_writes;
    device_count = 0;
    scanout_index = -1;
    bound = 0;
    families_without_port_record = 0;

    for (int s = 0; s < SCAN_SUBLASSES && device_count < GPU_MAX_DEVICES; s++) {
        int found = pci_find_class(3, subclasses[s], 0xff, bus, slot, function, 8);
        for (int i = 0; i < found && device_count < GPU_MAX_DEVICES; i++) {
            struct gpu_device *g = &devices[device_count];
            record(g, bus[i], slot[i], function[i], subclasses[s]);
            identify(g);
            /* Split into two lines on purpose: the serial logger truncates long
             * records, and the family and chip name are the parts a reader needs. */
            klog("gpu: PCI %u:%u.%u %x:%x sub=%x matched=%s",
                 g->bus, g->slot, g->function, g->vendor, g->device, g->subclass,
                 g->match ? g->match->family : "none");
            /* Deliberately "at detection", not "none": this line is written before the module pass
             * below has loaded anything, and a reader who greps one line of a boot log must not be able
             * to mistake the order of events for the state of the machine. */
            klog("gpu:   chip=%s engine=%s",
                 g->chip && g->chip[0] ? g->chip : "(no ID table match)",
                 g->bound ? "bound (in-tree port)" : "none at detection, CPU compositor");
            if (g->named_only)
                klog("gpu:   %x:%x is named by the PCI id registry only; no driver table in this tree "
                     "binds it, so nothing was loaded for it", g->vendor, g->device);
            if (g->reg_state == 1)
                klog("gpu:   read 0x%x from BAR0 of %x:%x; device registers are reachable%s",
                     g->reg_first, g->vendor, g->device,
                     g->reg_kernel_fallback ? " (the kernel, because no module was staged for it)" : "");
            else if (g->reg_state == 2)
                klog("gpu:   BAR0 of %x:%x is mapped but answers all-ones; the device is not responding",
                     g->vendor, g->device);
            device_count++;
        }
    }

    /* The scanning adapter is the one whose BAR already points at the framebuffer the
     * firmware validated.  Matching addresses is enough to know which device owns the
     * console, and unlike sizing a BAR it cannot disturb it.  This runs before the register
     * probe below precisely because the probe's "leave it to a driver" rule needs to know
     * whether this is the function the screen is being driven from. */
    for (int i = 0; i < device_count; i++) {
        for (int b = 0; b < 6; b++) {
            if (devices[i].bar[b] && devices[i].bar[b] == output.base) {
                devices[i].is_scanout = 1;
                scanout_index = i;
            }
        }
    }

    for (int i = 0; i < device_count; i++)
        if (!devices[i].match || devices[i].named_only || !devices[i].bound)
            probe_registers(&devices[i], handoff);
    config_writes_after = (int)pci_config_writes;
    if (config_writes_after != config_writes_before)
        klog("gpu: BUG: detection issued %u PCI config write(s); it must read only",
             (unsigned)(config_writes_after - config_writes_before));
    if (handoff)
        /* The stub's own numbers, in the log the user can read without opening the panel: "no module was
         * staged" means something different when two buses were scanned than when one empty bus was. */
        klog("gpu: the boot stub read %u function(s) on %u bus(es), %u display; module state %u",
             (unsigned)handoff->pci_functions_seen,
             (unsigned)handoff->pci_buses_scanned, (unsigned)handoff->pci_display_functions,
             (unsigned)handoff->module_state);
    klog("gpu: %u display function(s), %u ID rule(s) in %u family record(s), "
         "%u without a port record",
         device_count, (unsigned)gpu_match_id_total(), gpu_match_family_count(),
         (unsigned)families_without_port_record);
    /* Two counts, because the two tables make two different claims and a reader must be able to tell
     * them apart without opening the headers: ids that bind a driver, and ids that only name a chip. */
    klog("gpu: tables: %u id rules bind a driver; %u more ids name a chip that nothing in this tree "
         "covers", (unsigned)gpu_match_id_total(), (unsigned)gpu_registry_id_total());
    klog("gpu: scanout owner %s, %u PCI config write(s) issued",
         scanout_index >= 0 ? "identified by BAR address" : "not identified by address",
         (unsigned)(config_writes_after - config_writes_before));
    /* A family being *named* is not a driver.  The one module the firmware read for this machine's
     * chip is validated and exercised here; every other family's file stays on the disk. */
    if (!bound && handoff && handoff->module_state) {
        for (int i = 0; i < device_count; i++) {
            struct gpu_device *g = &devices[i];
            if (!gpu_module_eligible(g)) continue;
            if (strncmp(handoff->module_name, g->match->family,
                        strlen(g->match->family)) != 0) continue;
            if (gpu_module_bind(g, handoff) == 0) {
                /* Two different achievements, and the log must not blur them: a module that offered an
                 * engine and was verified by readback is "in use"; a module that read the chip and
                 * offered no drawing operation is identification, and saying "engine" there would be a
                 * claim this kernel has not measured. */
                if (!g->module_ops->fill)
                    klog("gpu: %x:%x.%x module %s identified this chip; it offers no engine, so the CPU "
                         "compositor keeps the screen", g->bus, g->slot, g->function,
                         g->chip ? g->chip : "this family");
                else
                    klog("gpu: %x:%x.%x %s engine in use from module %s", g->bus, g->slot, g->function,
                         g->match->family, g->chip ? g->chip : "this family");
                module_bound = 1;
                break;
            }
        }
    }
    if (!bound && !module_bound)
        klog("gpu: no engine bound, rendering stays on the CPU compositor");
    else if (identification_only_module_bound())
        klog("gpu: a driver read the chip and reported it; nothing was asked of the chip, so rendering "
             "stays on the CPU compositor");
    else if (!gpu_engine_drives_output())
        klog("gpu: engine drives its own aperture only: another function feeds the console, so output "
             "stays on the CPU");
    else
        klog("gpu: engine drives the console; solid output rectangles go through the GPU");
}

int gpu_device_count(void) { return device_count; }

int gpu_config_writes_during_detect(void)
{
    return config_writes_after - config_writes_before;
}

const struct gpu_device *gpu_device(int index)
{
    if (index < 0 || index >= device_count) return 0;
    return &devices[index];
}

const struct gpu_device *gpu_scanout_device(void)
{
    return scanout_index >= 0 ? &devices[scanout_index] : 0;
}

const struct gpu_driver *gpu_bound_driver(void) { return bound; }

/* The compositor boundary.  A statically linked port would answer first; today every family's
 * engine arrives as a module loaded from the boot disk, so the module's own table is what these
 * calls reach.  A negative return means "not available", which is the caller's cue to run its CPU
 * path - an engine is never required for correctness, only for speed. */
int gpu_engine_fill_rectangle(int x, int y, int width, int height, uint32_t color)
{
    if (bound && bound->ops && bound->ops->fill_rectangle)
        return bound->ops->fill_rectangle(x, y, width, height, color);
    const struct scos_gpu_engine_ops *ops = gpu_module_ops();
    if (!ops || !ops->fill) return -1;
    return ops->fill(ops->context, SCOS_GPU_SURFACE_FRONT, x, y, width, height, color);
}

int gpu_engine_screen_to_screen_blit(int dst_x, int dst_y, int width, int height,
                                     int src_x, int src_y)
{
    if (bound && bound->ops && bound->ops->screen_to_screen_blit)
        return bound->ops->screen_to_screen_blit(dst_x, dst_y, width, height, src_x, src_y);
    const struct scos_gpu_engine_ops *ops = gpu_module_ops();
    if (!ops || !ops->copy) return -1;
    return ops->copy(ops->context, SCOS_GPU_SURFACE_FRONT, dst_x, dst_y, SCOS_GPU_SURFACE_FRONT,
                     src_x, src_y, width, height);
}

int gpu_engine_wait_idle(void)
{
    if (bound && bound->ops && bound->ops->wait_idle) return bound->ops->wait_idle();
    const struct scos_gpu_engine_ops *ops = gpu_module_ops();
    if (!ops || !ops->wait_idle) return -1;
    return ops->wait_idle(ops->context);
}

int gpu_engine_retrace_wait(void)
{
    if (bound && bound->ops && bound->ops->retrace_wait) return bound->ops->retrace_wait();
    return -1;      /* no engine in this tree delivers a retrace; see the caps in gpu_ids.h */
}

int gpu_engine_move_display(int x, int y)
{
    if (bound && bound->ops && bound->ops->move_display) return bound->ops->move_display(x, y);
    (void)x; (void)y;
    return -1;      /* scanout ownership is not a 2D module capability */
}

int gpu_engine_available(void)
{
    /* "An engine is available" has to mean an engine that can be asked to draw, because that is the claim
     * the desktop raises a notification with.  A bound module always hands over an operations table - the
     * table is also how it describes the chip - so testing the pointer measured whether a driver had
     * loaded, not whether pixels could be moved.  On the machine this was written for, a module that reads
     * the chip and implements nothing made this return 1, and the notice that followed told the user a 2D
     * engine had been verified.  A fill pointer is the difference, and it is the one every drawing path
     * already tests before calling. */
    if (bound && bound->ops) return 1;
    const struct scos_gpu_engine_ops *m = gpu_module_ops();
    return m && m->fill ? 1 : 0;
}

static uint32_t engine_work_pixels, cpu_work_pixels;

void gpu_engine_note_work(uint32_t by_engine, uint32_t by_cpu)
{
    /* Saturating: these are a sanity check on the first few seconds of a boot, and a wrapped counter
     * would report less GPU work than happened, which is the one failure mode worth avoiding. */
    const uint64_t e = (uint64_t)engine_work_pixels + by_engine;
    const uint64_t c = (uint64_t)cpu_work_pixels + by_cpu;
    engine_work_pixels = e > 0xffffffffull ? 0xffffffffu : (uint32_t)e;
    cpu_work_pixels = c > 0xffffffffull ? 0xffffffffu : (uint32_t)c;
}

void gpu_engine_work_totals(uint32_t *by_engine, uint32_t *by_cpu)
{
    if (by_engine) *by_engine = engine_work_pixels;
    if (by_cpu) *by_cpu = cpu_work_pixels;
}

int gpu_engine_drives_output(void)
{
    /* A statically linked port is compiled for the console device, so it drives the output by
     * construction.  A module is bound to whichever function matched its family, which is not
     * necessarily the one feeding the screen. */
    if (bound && bound->ops) return 1;
    const struct gpu_device *owner = gpu_scanout_device();
    if (!owner || !owner->module_ops) return 0;
    /* A module bound for identification exposes no rectangle operation at all: saying its engine drives
     * the output would be false, and every drawing path would ask the card first for nothing. */
    return owner->module_ops->fill != 0;
}

/* Whether a loaded module deliberately exposes no engine.  Asked of the device list rather than of
 * gpu_module.c's status struct, because detection has to link - and be testable - without the loader. */
static int identification_only_module_bound(void)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].module_ops && !devices[i].module_ops->fill) return 1;
    return 0;
}

/* Link width and speed come out of this function's own PCI Express capability.  The register is defined
 * by PCI r3.0 7.5.3.15 rather than by any GPU driver, and reading a status field cannot change the
 * device, which is the rule this whole subsystem is held to.  The answer says what the slot actually
 * negotiated - a chip can be capable of more than the board it is plugged into will carry - so it is
 * worth more than anything a table could say, and it costs two config reads. */
int gpu_link_state(int index, unsigned *generation, unsigned *lanes)
{
    if (index < 0 || index >= device_count || !generation || !lanes) return 0;
    const struct gpu_device *g = &devices[index];
    u32 ptr = pci_read8(g->bus, g->slot, g->function, 0x34) & 0xfc;
    for (int hops = 0; hops < 48 && ptr >= 0x40; hops++) {
        u32 pair = pci_read32(g->bus, g->slot, g->function, (u8)ptr);
        if ((u8)(pair & 0xff) == 0x10) {              /* PCI Express capability */
            u32 lnksta = pci_read32(g->bus, g->slot, g->function, (u8)(ptr + 0x10));
            *generation = (lnksta >> 16) & 0xf;       /* Link Status: 1 = 2.5 GT/s, doubling per gen */
            *lanes = (lnksta >> 20) & 0x3f;
            return *generation != 0 && *lanes != 0;
        }
        ptr = (u8)((pair >> 8) & 0xfc);
    }
    return 0;
}

/* ------------------------------------------------------------ reporting ---- */

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
    char digits[12];
    int i = 0;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    char out[12];
    int n = 0;
    while (i) out[n++] = digits[--i];
    out[n] = 0;
    put(w, out);
}

static void put_hex(struct writer *w, uint16_t value)
{
    char text[5];
    hex4(text, value);
    put(w, text);
}

static void put_hex8(struct writer *w, uint32_t value)
{
    char text[9];
    hex4(text, (uint16_t)(value >> 16));
    hex4(text + 4, (uint16_t)value);
    put(w, text);
}

static void put_line(struct writer *w, const char *text)
{
    put(w, text);
    put(w, "\n");
}

void gpu_report(char *out, size_t capacity)
{
    struct writer w = {out, capacity};
    char number[24];

    put_line(&w, "GPU detection (exact device matching; no BAR sizing, no modeset)");
    put(&w, "Families named from upstream tables: ");
    put_number(&w, (unsigned)gpu_match_family_count());
    put(&w, ", device ID rules: ");
    put_number(&w, (unsigned)gpu_match_id_total());
    put_line(&w, "");
    if (!device_count) {
        put_line(&w, "No PCI display function found; firmware scanout retained.");
        return;
    }
    for (int i = 0; i < device_count && i < REPORT_DEVICE_LIMIT; i++) {
        const struct gpu_device *g = &devices[i];
        put(&w, "- ");
        put(&w, gpu_vendor_name(g->vendor));
        put(&w, " ");
        put_hex(&w, g->vendor);
        put(&w, ":");
        put_hex(&w, g->device);
        number[0] = 0;
        put(&w, " at ");
        fmt_u32(number, g->bus);
        put(&w, number);
        put(&w, ":");
        fmt_u32(number, g->slot);
        put(&w, number);
        put(&w, ".");
        fmt_u32(number, g->function);
        put(&w, number);
        if (g->is_scanout) put(&w, " (scanout)");
        put(&w, "\n");
        {
            unsigned generation = 0, lanes = 0;
            char link[40];
            put(&w, "  link: ");
            if (gpu_link_state(i, &generation, &lanes)) {
                fmt_u32(link, generation);
                put(&w, "PCIe gen");
                put(&w, link);
                put(&w, " x");
                fmt_u32(link, lanes);
                put_line(&w, link);
            } else {
                put_line(&w, "no PCI Express capability on this function (conventional PCI slot)");
            }
        }
        if (!g->match) {
            put(&w, "  match: none - no upstream table binds ");
            put_hex(&w, g->vendor);
            put(&w, ":");
            put_hex(&w, g->device);
            put_line(&w, "; treated as an unmatched display adapter");
            continue;
        }
        put(&w, "  family: ");
        put(&w, g->match->family);
        put(&w, ", chip: ");
        put(&w, g->chip ? g->chip : "(unnamed row in upstream table)");
        put(&w, ", source: ");
        put(&w, g->match->source);
        put_line(&w, "");
        if (g->match->note) {
            put(&w, "  table note: ");
            put(&w, g->match->note);
            put_line(&w, "");
        }
        put(&w, "  upstream entry points: ");
        put(&w, g->match->features);
        put_line(&w, "");
        put(&w, "  device registers: ");
        switch (g->reg_state) {
        case 1:
            put(&w, "BAR0 at 0x");
            put_hex8(&w, (uint32_t)(g->reg_base >> 32));
            put_hex8(&w, (uint32_t)g->reg_base);
            put(&w, " read: first dword 0x");
            put_hex8(&w, g->reg_first);
            put(&w, ", second 0x");
            put_hex8(&w, g->reg_second);
            if (g->vendor == 0x10de) {
                /* Fields as NVIDIA publishes them (see the comment on probe_registers): the [31:20]
                 * selector is the chip, [7:4] and [3:0] are the major and minor revision.  Printed
                 * beside the raw dword rather than instead of it, because a decode is a claim and the
                 * word is the measurement. */
                put(&w, " (NV_PMC_BOOT_0: chip selector 0x");
                put_hex8(&w, (g->reg_first >> 20) & 0xfff);
                put(&w, ", architecture 0x");
                put_hex8(&w, (g->reg_first >> 24) & 0x1f);
                put(&w, ", implementation 0x");
                put_hex8(&w, (g->reg_first >> 20) & 0xf);
                put(&w, ", revision ");
                {
                    static const char major[] = "0123456789ABCDEF";
                    char tail[8];
                    int n = 0;
                    tail[n++] = major[(g->reg_first >> 4) & 0xf];
                    tail[n++] = '.';
                    u32 minor = g->reg_first & 0xf;
                    tail[n++] = (char)('0' + minor);
                    tail[n] = 0;
                    put(&w, tail);
                }
                put(&w, ")");
            }
            if (g->reg_kernel_fallback)
                /* Named as the kernel's own reading, so a report cannot be mistaken for a driver's work:
                 * this line is printed when the function matches a driver family and the disk's module for
                 * it never reached memory. */
                put(&w, " (read by the kernel itself: this function matches a driver family, but no module "
                        "was staged for it)");
            put_line(&w, "");
            break;
        case 2:
            put_line(&w, "BAR0 mapped, but every read returned 0xffffffff - the device is not answering");
            break;
        case 3:
            put_line(&w, "BAR0 is outside the kernel's mapping capacity, so it was not read");
            break;
        case 4:
            put_line(&w, "not read: memory decoding is disabled in this function's command register, "
                         "and detection does not write configuration space");
            break;
        case 5:
            put_line(&w, "not read: BAR0 is the frame buffer aperture the kernel already maps for scanout");
            break;
        default:
            put_line(&w, "not read (a driver module may claim this function, or no BAR0 was reported)");
            break;
        }
        const struct gpu_driver *port = gpu_port_for(g->match->family);
        put(&w, "  SCos port: ");
        if (g->module_ops && g->module_ops->fill)
            put_line(&w, "driver module loaded from storage for this family; engine verified by device "
                         "readback");
        else if (g->module_ops)
            put_line(&w, "driver module bound for this family reads the chip and describes it; it offers "
                         "no engine operation, so nothing about drawing has changed");
        else
            put_line(&w, port && port->state != GPU_PORT_NONE ? "bound" : "not ported yet");
        /* The gap text says what was missing before a driver existed.  Once a module is bound for this
         * function it would read as a contradiction, so it is left to the families that really are
         * still waiting. */
        if (port && port->gap && !g->module_ops) {
            put(&w, "  before GPU work is possible: ");
            put(&w, port->gap);
            put_line(&w, "");
        }
        /* Which path this function's pixels actually take.  A module bound on a function that does not
         * feed the console is real and verified, but it is not what the user sees, so the line says
         * both halves instead of claiming an acceleration that the display cannot show. */
        if (g->module_ops && !g->module_ops->fill)
            put_line(&w, "  path in use: the CPU compositor paints this screen; the module read this "
                         "function's registers and reported them, and asked the chip for nothing else");
        else if (g->module_ops)
            put_line(&w, gpu_engine_drives_output()
                ? "  path in use: module engine paints this function's scanout; solid rectangles skip "
                  "the CPU copy"
                : "  path in use: module engine verified on this function, but another PCI function "
                  "feeds the console, so the CPU compositor paints it");
        else
            put_line(&w, g->bound ? "  path in use: bound engine for supported operations"
                                 : "  path in use: CPU compositor into the firmware scanout");
    }
    if (device_count > REPORT_DEVICE_LIMIT) {
        put(&w, "  (");
        put_number(&w, (unsigned)(device_count - REPORT_DEVICE_LIMIT));
        put_line(&w, " further display function(s) reported in the boot log only)");
    }
    {
        /* The number the whole driver effort is judged by: how many pixels the card painted versus how
         * many the CPU still copied one by one.  It is printed on every boot, including boots where a
         * module loaded correctly but never drew a thing, because "a driver is present" and "the GPU is
         * doing the drawing" are different claims and only the second one matters to the user. */
        uint32_t engine_px = 0, cpu_px = 0;
        gpu_engine_work_totals(&engine_px, &cpu_px);
        put(&w, "  pixels painted: ");
        put_number(&w, engine_px);
        put(&w, " by the GPU's 2D engine, ");
        put_number(&w, cpu_px);
        put_line(&w, " by the CPU compositor");
        if (!engine_px)
            put_line(&w, "  (no GPU drawing yet: the CPU copied every pixel of this boot)");
    }
    put(&w, "  detection issued ");
    put_number(&w, (unsigned)gpu_config_writes_during_detect());
    put_line(&w, " PCI config-space write(s); 0 means nothing was reprogrammed");
}
