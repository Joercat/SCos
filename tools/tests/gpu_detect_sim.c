/* Host harness for SCos' GPU auto-detection: real sources, simulated PCI bus.
 *
 * This compiles the *shipped* detection code (`kernel/drivers/gpu/gpu_detect.c`,
 * `gpu_ports.c`, `gpu_tables.c` and the generated `gpu_ids.h`) unchanged and runs it
 * against synthetic PCI functions, because no emulator here can present a Radeon, a
 * Matrox G450 or an i810 to the kernel: QEMU's x86 machines have no such device model
 * and cannot re-identify one.  So the matching rules - the part that decides which
 * family a chip belongs to, and which part is safest to test without hardware - are
 * verified here, on every family and both ends of every table, while QEMU verifies the
 * same code against real emulated PCI functions (see `test_gpu_detect.py`).
 *
 * What is asserted, and why each one matters:
 *   - every family in the generated table is detected from its own oldest and newest
 *     device ID, and reports the chip name from that row;
 *   - the family's class predicate is respected (an ID matched at the wrong subclass is
 *     not a match, exactly as upstream's `continue` says);
 *   - a device whose vendor or device ID is absent from every table binds to nothing;
 *   - no engine is bound, and no PCI configuration write is issued, by detection;
 *   - every family has a port record, and no record claims a state without ops.
 *
 * Build and run through `tools/tests/test_gpu_detect.py`.
 */
#include "scos.h"

/* No <string.h>: `scos.h` declares `strstr` as returning `const char *`, which the host
 * header would contradict, and the shipped sources only need `strcmp`.  The harness
 * therefore supplies the two string functions it and the kernel objects call. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *find_substring(const char *haystack, const char *needle)
{
    size_t n = 0;
    while (needle[n]) n++;
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (needle[i] && h[i] == needle[i]) i++;
        if (!needle[i]) return h;
    }
    return NULL;
}

/* ------------------------------------------------------------ fake bus ---- */

#define MAX_SIM 64

struct sim_device {
    uint8_t bus, dev, fn, class_base, class_sub, rev;
    uint16_t vendor, device;
    uint32_t bar[6];
    uint32_t command;
};

static struct sim_device sim[MAX_SIM];
static int sim_count;

static void sim_add(struct sim_device d)
{
    if (sim_count >= MAX_SIM) { fprintf(stderr, "sim: too many devices\n"); exit(2); }
    sim[sim_count++] = d;
}

static void sim_reset(void)
{
    sim_count = 0;
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    for (int i = 0; i < sim_count; i++) {
        const struct sim_device *d = &sim[i];
        if (d->bus != bus || d->dev != dev || d->fn != fn) continue;
        switch (off) {
        case 0x00: return ((uint32_t)d->device << 16) | d->vendor;
        case 0x04: return d->command;
        case 0x08: return ((uint32_t)d->class_base << 24) | ((uint32_t)d->class_sub << 16)
                       | ((uint32_t)d->rev << 8);
        case 0x0c: return 0;                     /* header type 0, latency ignored */
        case 0x10: case 0x14: case 0x18: case 0x1c: case 0x20: case 0x24:
            return d->bar[(off - 0x10) / 4];
        default: return 0;
        }
    }
    return 0xffffffffu;                          /* nothing there: the real bus reads ~0 */
}

int pci_find_class(uint8_t cls, uint8_t sub, uint8_t pi, uint8_t *b, uint8_t *d,
                   uint8_t *f, int max)
{
    (void)pi;
    int n = 0;
    for (int i = 0; i < sim_count && n < max; i++) {
        const struct sim_device *s = &sim[i];
        if (s->class_base != cls || (sub != 0xff && s->class_sub != sub)) continue;
        b[n] = s->bus;
        d[n] = s->dev;
        f[n] = s->fn;
        n++;
    }
    return n;
}

/* The kernel-wide write counter, normally kept by kernel/drivers/pci.c.  The simulated
 * bus increments it exactly as the real one does, so if any shipped code ever writes
 * configuration space during detection the two assertions at the end fire. */
unsigned pci_config_writes;
static int writes_seen;

static void sim_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t value)
{
    writes_seen++;
    pci_config_writes++;
    for (int i = 0; i < sim_count; i++) {
        struct sim_device *d = &sim[i];
        if (d->bus != bus || d->dev != dev || d->fn != fn) continue;
        if (off == 4) d->command = value;
        if (off >= 0x10 && off <= 0x24 && !((off - 0x10) & 3))
            d->bar[(off - 0x10) / 4] = value;
    }
    printf("SIM: config write to %u:%u.%u offset %u = %x\n", bus, dev, fn, off, value);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t value)
{
    sim_write(bus, dev, fn, off, value);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t value)
{
    sim_write(bus, dev, fn, off, value);
}

void klog(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    printf("[klog] %s\n", text);
}

void fmt_u32(char *out, uint32_t value)
{
    char digits[12];
    int i = 0, n = 0;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (i) out[n++] = digits[--i];
    out[n] = 0;
}

/* ------------------------------------------------------------- checks ---- */

static int failures;

static void expect(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static struct boot_framebuffer fake_fb = {
    .base = 0xe0000000ull, .size = 8u << 20,
    .width = 1280, .height = 720, .stride = 1280,
    .red = 0x00ff0000u, .green = 0x0000ff00u, .blue = 0x000000ffu,
};

static int run_with(struct sim_device *devices, int count)
{
    sim_reset();
    for (int i = 0; i < count; i++) sim_add(devices[i]);
    gpu_init(&fake_fb);
    return gpu_device_count();
}

static const struct gpu_device *device_for(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < gpu_device_count(); i++) {
        const struct gpu_device *g = gpu_device(i);
        if (g->vendor == vendor && g->device == device) return g;
    }
    return NULL;
}

/* The module loader itself is not linked into this harness: it needs the page allocator, the
 * device mapper and BAR sizing, none of which exist outside a booted kernel.  Those predicates are
 * covered by the packer's --verify and by the QEMU layer, which boots a real image; here they are
 * stubbed so gpu_detect.c can still be exercised for detection. */
static struct boot_handoff sim_handoff;
const struct boot_handoff *kernel_boot_handoff(void)
{
    sim_handoff.module_state = 0;      /* no module on the host: detection only, as designed */
    sim_handoff.module_bytes = 0;
    return &sim_handoff;
}

int gpu_module_bind(struct gpu_device *device, const struct boot_handoff *handoff)
{
    (void)device; (void)handoff;
    return -1;
}
const struct scos_gpu_engine_ops *gpu_module_ops(void) { return 0; }
void gpu_module_report(char *out, size_t capacity, const struct boot_handoff *handoff)
{
    (void)handoff;
    if (capacity) out[0] = 0;
}

int main(void)
{
    const int families = gpu_match_family_count();
    printf("harness: %d family record(s), %d device ID rule(s)\n",
           families, gpu_match_id_total());
    expect(families == 15, "the generated table covers the 15 surveyed families");

    /* One device per family, at both ends of that family's own table.  Feeding the
     * oldest and newest ID is the point: an off-by-one or a truncated parse in the
     * generator would show up as a missing match on one of the two ends. */
    int cases = 0;
    for (int f = 0; f < families; f++) {
        const struct gpu_match *m = gpu_match_family(f);
        if (!m || !m->id_count) continue;
        const struct gpu_pci_id *ends[2] = {&m->ids[0], &m->ids[m->id_count - 1]};
        for (int e = 0; e < 2; e++) {
            uint8_t sub = m->class_sub_a == 0xff ? 0 : m->class_sub_a;
            struct sim_device d = {
                .bus = 0, .dev = (uint8_t)(5 + cases % 20), .fn = 0,
                .class_base = 3, .class_sub = sub, .rev = 0,
                .vendor = ends[e]->vendor, .device = ends[e]->device,
                .bar = {0xe0000008u, 0, 0, 0, 0, 0}, .command = 7,
            };
            char note[192];
            snprintf(note, sizeof note, "%s binds its %s ID %04x:%04x", m->family,
                     e ? "newest" : "oldest", d.vendor, d.device);
            run_with(&d, 1);
            const struct gpu_device *g = device_for(d.vendor, d.device);
            if (!g || !g->match || strcmp(g->match->family, m->family) != 0)
                printf("  detail: devices=%d g=%p match=%s class_sub=%u\n",
                       gpu_device_count(), (void *)g,
                       (g && g->match) ? g->match->family : (g ? "none" : "missing"),
                       g ? g->subclass : 0);
            expect(g && g->match && strcmp(g->match->family, m->family) == 0, note);
            expect(g && g->match && g->match->id_count == m->id_count,
                   "matched through the same table");
            if (g && g->match && g->chip && ends[e]->name &&
                strcmp(g->chip, ends[e]->name) != 0) {
                printf("FAIL: %s chip name mismatch: %s vs %s\n", m->family, g->chip,
                       ends[e]->name);
                failures++;
            }
            expect(g && !g->bound, "no family reports a bound engine");
            expect(g && gpu_bound_driver() == NULL, "no driver is bound anywhere");
            cases++;
        }
    }
    printf("harness: %d positive match case(s) from the generated tables\n", cases);

    /* Negative cases.  Each one has to bind to *nothing*: a wrong vendor, a wrong
     * device, or the right ID at a subclass the upstream driver's own loop skips. */
    struct sim_device negative[] = {
        {0, 12, 0, 3, 0, 0, 0x8086, 0x4c8b, {0xe0000008u}, 7},   /* Rocket Lake UHD 730 */
        {0, 13, 0, 3, 0, 0, 0x1002, 0x7479, {0}, 7},             /* RDNA3, not in table */
        {0, 14, 0, 3, 0, 0, 0x1234, 0x1111, {0xe0000008u}, 7},   /* QEMU Bochs scanout  */
        {0, 15, 0, 3, 0, 0, 0x10de, 0x2230, {0}, 7},             /* GA102, past NV48    */
        {0, 16, 0, 3, 2, 0, 0x8086, 0x29c2, {0}, 7},             /* Intel ID, 3D subclass */
    };
    for (unsigned i = 0; i < sizeof negative / sizeof negative[0]; i++) {
        struct sim_device probe = negative[i];
        const struct gpu_match *claimed = NULL;
        /* Skip the case if the ID is in fact present upstream with no class test -
         * the harness reads the tables, it does not second-guess them.  Rows carry
         * their own vendor because a family may pair several of them. */
        for (int f = 0; f < families && !claimed; f++) {
            const struct gpu_match *m = gpu_match_family(f);
            if (!m || !m->id_count || m->class_base != 0xff) continue;
            for (unsigned k = 0; k < m->id_count; k++) {
                if (m->ids[k].vendor == probe.vendor && m->ids[k].device == probe.device) {
                    claimed = m;
                    break;
                }
            }
        }
        if (claimed) continue;
        run_with(&probe, 1);
        const struct gpu_device *g = device_for(probe.vendor, probe.device);
        char note[128];
        snprintf(note, sizeof note, "%04x:%04x is reported unmatched", probe.vendor,
                 probe.device);
        expect(g && g->match == NULL, note);
        expect(g && g->chip == NULL, "unmatched devices carry no chip name");
    }
    /* And the same Intel ID at the subclass its driver does accept, so the class test
     * above is a real gate and not an accident of the table. */
    {
        struct sim_device d = {0, 17, 0, 3, 0, 0, 0x8086, 0x29c2, {0xe0000008u}, 7};
        run_with(&d, 1);
        const struct gpu_device *g = device_for(0x8086, 0x29c2);
        expect(g && g->match && strcmp(g->match->family, "intel_extreme") == 0,
               "the same ID at subclass 0x00 does match intel_extreme");
    }

    /* Scanout ownership must be found from the BAR address, and must survive a second
     * adapter being present. */
    {
        struct sim_device pair[2] = {
            {0, 1, 0, 3, 0, 0, 0x1002, 0x5b60, {0}, 7},
            {0, 2, 0, 3, 0, 0, 0x8086, 0x29c2, {fake_fb.base | 8u}, 7},
        };
        run_with(pair, 2);
        const struct gpu_device *owner = gpu_scanout_device();
        expect(owner && owner->vendor == 0x8086 && owner->device == 0x29c2,
               "the adapter whose BAR holds the framebuffer is named as scanout owner");
        expect(gpu_device_count() == 2, "both display functions are recorded");
    }

    /* Registry invariants: a family without a port record would be silently unusable,
     * and a record may not advertise a state it has no code for. */
    for (int f = 0; f < families; f++) {
        const struct gpu_match *m = gpu_match_family(f);
        const struct gpu_driver *port = gpu_port_for(m->family);
        char note[96];
        snprintf(note, sizeof note, "%s has a port record", m->family);
        expect(port != NULL, note);
        if (!port) continue;
        expect(port->state == GPU_PORT_NONE || port->ops != NULL,
               "a port record with a state also has engine ops");
        expect(port->state != GPU_PORT_NONE || !port->ops || port->gap != NULL,
               "an unported family states what is missing");
    }

    /* Capability bits must be a summary of the measured text, so the report and the
     * bits cannot disagree about a family. */
    for (int f = 0; f < families; f++) {
        const struct gpu_match *m = gpu_match_family(f);
        if (m->upstream.vsync)
            expect(find_substring(m->features, "retrace:direct") != NULL,
                   "vsync bit implies a direct retrace status");
        if (m->upstream.pan)
            expect(find_substring(m->features, "pan:direct") != NULL,
                   "pan bit implies a direct MOVE_DISPLAY status");
        expect(m->features[0] && m->source[0], "every family names its upstream source");
    }

    expect(writes_seen == 0, "detection issued no PCI configuration write");
    expect(gpu_config_writes_during_detect() == 0,
           "the detection write count reads as zero");

    if (failures) {
        printf("harness: %d failure(s)\n", failures);
        return 1;
    }
    printf("harness: all detection checks passed\n");
    return 0;
}
