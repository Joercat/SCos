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

/* A PCI Express capability, laid out the way PCI r3.0 describes it: the capability list is entered at
 * 0x40, the first entry is id 0x10, and Link Status is the 16-bit register at +0x12 - which the kernel
 * reads as the 32-bit word at +0x10, so the two fixtures below pack speed and width into that word the
 * way the bus does.  Held beside the device list rather than inside it, so the many positional
 * initializers above stay the shape they are. */
static uint8_t sim_express[MAX_SIM];
static uint32_t sim_lnksta[MAX_SIM];

static void sim_set_link(int index, int express, uint32_t lnksta)
{
    sim_express[index] = (uint8_t)express;
    sim_lnksta[index] = lnksta;
}

static struct sim_device sim[MAX_SIM];
static int sim_count;

/* The kernel's device mapper, replaced here by one fabricated page of content.  The point of the
 * register probe is that a value comes back out of an aperture the kernel mapped itself, so the
 * harness has to be able to say what that aperture answers - including "all-ones", which on real
 * hardware means the function is not responding, and which must never be reported as a chip id. */
static uint32_t sim_mmio[1024];
static uint64_t sim_mmio_phys;
static int sim_maps;
static int sim_map_fails;

void *device_map(uint64_t physical, uint64_t bytes, int write_combine, uint64_t *mapped_bytes)
{
    (void)write_combine;
    if (sim_map_fails || bytes > sizeof(sim_mmio)) return 0;
    sim_maps++;
    sim_mmio_phys = physical;
    if (mapped_bytes) *mapped_bytes = bytes;
    return sim_mmio;
}

static void sim_add(struct sim_device d)
{
    if (sim_count >= MAX_SIM) { fprintf(stderr, "sim: too many devices\n"); exit(2); }
    sim[sim_count++] = d;
}

static void sim_reset(void)
{
    sim_count = 0;
    for (unsigned i = 0; i < MAX_SIM; i++) { sim_express[i] = 0; sim_lnksta[i] = 0; }
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
        case 0x40: return sim_express[i] ? 0x10u : 0;    /* cap id 0x10, end of the list */
        case 0x50: return sim_express[i] ? sim_lnksta[i] : 0;  /* Link Control / Link Status */
        default: return 0;
        }
    }
    return 0xffffffffu;                          /* nothing there: the real bus reads ~0 */
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    for (int i = 0; i < sim_count; i++) {
        const struct sim_device *d = &sim[i];
        if (d->bus != bus || d->dev != dev || d->fn != fn) continue;
        if (off == 0x34) return sim_express[i] ? 0x40 : 0;
        return (uint8_t)(pci_read32(bus, dev, fn, (uint8_t)(off & 0xfc)) >> ((off & 3) * 8));
    }
    return 0xff;
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

/* Ids newer than every driver in this tree, taken from the generated naming table.  What is being
 * asserted is the whole design of that table in six lines: it must identify the chip, and it must
 * change nothing else - no port bound, no module eligible, and `named_only` set so that every later
 * stage can tell a name from a claim. */
static const struct {
    uint16_t vendor, device;
    const char *family, *name;
} named_rows[] = {
    /* GeForce RTX 5050 (0x2d83) and 5090 (0x2b85) were here until drivers/gpu/nvidia existed: the
     * Blackwell ids are now rows in the `nvidia` binding table, so they name a chip *and* select a
     * module, and are checked as positive cases below instead.  This row and the two after it stay
     * naming-only: no driver in this tree, and no SCos module, reads those chips. */
    {0x10de, 0x1b06, "nvidia", "GP102 [GeForce GTX 1080 Ti] (Pascal)"},
    {0x1002, 0x744c, "radeon_hd", "Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]"},
    {0x8086, 0x56a1, "intel_extreme", "DG2 [Arc A750]"},
};

int main(void)
{
    const int families = gpu_match_family_count();
    printf("harness: %d family record(s), %d device ID rule(s)\n",
           families, gpu_match_id_total());
    /* Two different provenances live in one table, and the distinction has to survive regeneration:
     * fifteen records are generated from pinned upstream driver sources, and exactly one — Cirrus —
     * is hand-authored here because no upstream 2D driver exists in this tree to read an ID table
     * from.  Asserting the split, not just the total, is what stops a `--haiku` regeneration from
     * silently dropping the local family (it would leave 15 upstream + 0 local) or from somebody
     * quietly adding an unsourced record (16 upstream would fail here, not just the count). */
    int upstream_records = 0, local_records = 0;
    for (int f = 0; f < families; f++) {
        const struct gpu_match *m = gpu_match_family(f);
        if (!m) continue;
        if (m->note && strstr(m->note, "hand-authored")) local_records++;
        else upstream_records++;
    }
    expect(families == 16, "the table holds 15 generated families plus the hand-authored one");
    expect(upstream_records == 15, "every surveyed upstream family still has a generated record");
    expect(local_records == 1, "exactly one record is hand-authored, and it is marked as such");
    {
        int cirrus = 0;
        for (int f = 0; f < families; f++) {
            const struct gpu_match *m = gpu_match_family(f);
            if (m && m->note && strstr(m->note, "hand-authored"))
                cirrus = m->id_count > 0 && m->upstream.engine2d == 1 && m->upstream.fill == 1 &&
                         m->upstream.blit == 1 && m->upstream.pan == 0 && m->upstream.cursor == 0 &&
                         m->upstream.overlay == 0 && m->upstream.span == 0 &&
                         m->upstream.modeset == 0 && m->upstream.dpms == 0 &&
                         m->upstream.vsync == 0 && strstr(m->family, "cirrus") != 0;
        }
        expect(cirrus, "the hand-authored record is Cirrus, and claims only what its module does");
    }
    /* The other kind of local content: rows added *inside* an upstream family's array, which is how a
     * chip moves from "named, nothing binds it" to "bound, and a module is offered for it".  Counted
     * from the table itself, and checked against the module's own claim list, so the two cannot drift
     * apart and a regeneration cannot drop the rows without this failing. */
    {
        const struct gpu_match *nvidia = NULL;
        for (int f = 0; f < families; f++) {
            const struct gpu_match *m = gpu_match_family(f);
            if (m && strcmp(m->family, "nvidia") == 0) nvidia = m;
        }
        expect(nvidia && nvidia->note && strstr(nvidia->note, "id rows added by SCos"),
               "the nvidia family record says its id list was extended by SCos, not upstream");
        unsigned blackwell = 0;
        if (nvidia)
            for (unsigned k = 0; k < nvidia->id_count; k++)
                if (nvidia->ids[k].name && strncmp(nvidia->ids[k].name, "GB2", 3) == 0 &&
                    nvidia->ids[k].device >= 0x2b85 && nvidia->ids[k].device <= 0x2f18)
                    blackwell++;
        expect(blackwell == 19, "the 19 hand-authored rows are the Blackwell display ids");
        if (nvidia) {
            const struct gpu_pci_id *last = &nvidia->ids[nvidia->id_count - 1];
            expect(last->device == 0x2f18, "the family's newest row is a Blackwell one, not an old one");
        }
        int found_5050 = 0;
        if (nvidia)
            for (unsigned k = 0; k < nvidia->id_count; k++)
                if (nvidia->ids[k].vendor == 0x10de && nvidia->ids[k].device == 0x2d83)
                    found_5050 = nvidia->ids[k].name &&
                                 strcmp(nvidia->ids[k].name, "GB207 [GeForce RTX 5050]") == 0;
        expect(found_5050, "10de:2d83 is bound by a row naming it GB207 [GeForce RTX 5050]");
    }

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
            expect(g && !g->named_only, "a row from the driver's own table is not a naming row");
            expect(g && gpu_bound_driver() == NULL, "no driver is bound anywhere");
            cases++;
        }
    }
    printf("harness: %d positive match case(s) from the generated tables\n", cases);

    /* Negative cases.  Each one has to bind to *nothing*: a wrong vendor, a wrong
     * device, or the right ID at a subclass the upstream driver's own loop skips. */
    struct sim_device negative[] = {
        {0, 12, 0, 3, 0, 0, 0x8086, 0x4c8b, {0xe0000008u}, 7},   /* named by the registry now */
        {0, 18, 0, 3, 0, 0, 0x10de, 0x0006, {0}, 7},              /* NVIDIA MPS: in neither table */
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
        if (gpu_match_named_only(probe.vendor, probe.device, probe.class_sub)) {
            /* The driver tables do not bind this id, but the naming table knows the chip, so
             * "unmatched" would be the wrong expectation: it has to be named, marked, and bind
             * nothing.  Asked here rather than kept in a hand-written list, so that refreshing the
             * registry snapshot cannot leave a stale negative case claiming an id is unknown. */
            run_with(&probe, 1);
            const struct gpu_device *g = device_for(probe.vendor, probe.device);
            expect(g && g->match && g->named_only, "known by name only, and marked as such");
            expect(g && g->chip && g->chip[0], "a name-only device still carries the chip's name");
            expect(g && !g->bound, "a name binds no engine");
            expect(g && !gpu_module_eligible(g), "a name selects no module");
            continue;
        }
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
    int named_cases = 0;
    for (unsigned i = 0; i < sizeof named_rows / sizeof *named_rows; i++) {
        struct sim_device d = {
            .bus = 0, .dev = 6, .fn = 0, .class_base = 3, .class_sub = 0, .rev = 0,
            .vendor = named_rows[i].vendor, .device = named_rows[i].device,
            .bar = {0xe0000008u, 0, 0, 0, 0, 0}, .command = 7,
        };
        char note[192];
        snprintf(note, sizeof note, "%04x:%04x named by the registry as %s",
                 d.vendor, d.device, named_rows[i].family);
        run_with(&d, 1);
        const struct gpu_device *g = device_for(d.vendor, d.device);
        expect(g && g->match && strcmp(g->match->family, named_rows[i].family) == 0, note);
        expect(g && g->named_only, "the matcher marks a naming row as naming, not binding");
        expect(g && !g->bound, "a naming row binds no engine");
        expect(g && gpu_bound_driver() == NULL, "a naming row binds nothing anywhere");
        snprintf(note, sizeof note, "the chip name is the registry's own row: %s", named_rows[i].name);
        expect(g && g->chip && strcmp(g->chip, named_rows[i].name) == 0, note);
        expect(g && !gpu_module_eligible(g), "a naming row cannot put a module on the table");
        named_cases++;
    }
    printf("harness: %d naming-row case(s) checked\n", named_cases);

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

    /* A card of the generation this machine has puts its frame buffer in a 64-bit BAR *above* 4 GiB.
     * Ownership is decided by comparing that address with the one UEFI reported, so the fold of the two
     * config words into one 64-bit value is load-bearing: without it the machine knows what chip it owns
     * and cannot say which function the firmware is scanning out from, which is the difference between
     * "driver not loaded" and "driver refused". */
    {
        /* The same layout, with the 1 MiB MMIO BAR the real card also has: this is where the kernel
         * stops at "named", so the register probe is the whole difference between naming a chip and
         * having touched it.  0x2b0000a1 is a Blackwell-shaped NV_PMC_BOOT_0: chip id 0x2b0, revision 0xa. */
        uint64_t keep = fake_fb.base;
        uint64_t high_half = 0x100000000ull;
        struct sim_device pair[2] = {
            {0, 1, 0, 3, 0, 0, 0x10de, 0x2d83,
             {0xe0100000u, 0x0000000cu, (uint32_t)(high_half >> 32), 0u, 0u, 0u}, 7},
            {0, 2, 0, 3, 0, 0, 0x8086, 0x29c2, {0xe0000008u}, 7},
        };
        fake_fb.base = high_half;             /* the LFB begins at that BAR */
        run_with(pair, 2);
        const struct gpu_device *owner = gpu_scanout_device();
        expect(owner && owner->vendor == 0x10de && owner->device == 0x2d83,
               "a 64-bit BAR above 4 GiB still names the scanout owner");
        expect(owner && owner->bar[1] == high_half, "the upper config word is folded into the BAR");
        /* This is the state the user's machine is in: the chip is bound to a family by a row SCos
         * authored, and a module is offered for it, so the kernel's own register probe stands down and
         * leaves the read to drivers/gpu/nvidia - mapping the same aperture twice to ask the same
         * question twice would be worse than not asking. */
        expect(owner && !owner->named_only, "the Blackwell id binds the nvidia family, not just a name");
        expect(owner && owner->match && strcmp(owner->match->family, "nvidia") == 0,
               "the binding names the family whose module is on the disk");
        expect(owner && gpu_module_eligible(owner), "a bound id puts the nvidia module on the table");
        /* The negotiated link, read out of the function's own capability: what a card can do is in the
         * tables, what it is actually doing on this board is only in the silicon. */
        /* Set after the run: registering devices resets the simulated bus, exactly as a real bus
         * forgets nothing but is only queried when something asks.  The link registers are read by the
         * report, not by detection, and that ordering is part of what this case pins down. */
        run_with(pair, 2);
        owner = gpu_scanout_device();
        sim_set_link(0, 1, (4u << 16) | (16u << 20));    /* gen4 (16 GT/s per lane), 16 lanes */
        unsigned generation = 0, lanes = 0;
        expect(gpu_link_state(0, &generation, &lanes), "the capability is found and decoded");
        expect(generation == 4 && lanes == 16, "gen4 x16 is what the two fields say");
        expect(!gpu_link_state(1, &generation, &lanes), "a function with no Express cap reports none");
        {
            char text[4096];
            gpu_report(text, sizeof(text));
            expect(find_substring(text, "link: PCIe gen4 x16") != NULL,
                   "the report prints the negotiated link for the card that has one");
            expect(find_substring(text, "no PCI Express capability on this function") != NULL,
                   "and says so plainly for the one that does not");
        }
        expect(owner && owner->reg_state == 0, "the kernel does not map a BAR the module was loaded for");
        expect(sim_maps == 0, "no aperture was mapped by the kernel for either function here");
        sim_maps = 0;
        sim_mmio[0] = 0x2b0000a1u;
        sim_mmio[1] = 0x00000000u;
        /* The same layout at an NVIDIA id that is *named* but bound by nobody - a family match with no
         * module claim, which is the only shape left where the kernel's own probe is the reader.  That
         * is where the register read and NVIDIA's documented field decode are verified. */
        pair[0].device = 0x1b06;
        run_with(pair, 2);
        owner = gpu_scanout_device();
        expect(owner && owner->reg_state == 1, "the unclaimed function's BAR0 was read");
        expect(owner && owner->reg_first == 0x2b0000a1u, "the value read is the one the device answered");
        expect(sim_maps == 1, "exactly one aperture was mapped, and only for the unclaimed function");
        expect(sim_mmio_phys == 0xe0100000ull, "the probe used BAR0, not the frame buffer BAR");
        expect(writes_seen == 0, "the register probe wrote no configuration space");
        {
            char text[4096];
            gpu_report(text, sizeof(text));
            expect(find_substring(text, "device registers: BAR0 at 0x00000000e0100000 read: first dword 0x2b0000a1") != NULL,
                   "the report quotes the address and the value that were read");
            expect(find_substring(text, "chip selector 0x000002b0, architecture 0x0000000b, "
                                        "implementation 0x00000000, revision A.1") != NULL,
                   "the NVIDIA fields are decoded where NVIDIA documents them: selector [31:20], "
                   "architecture [28:24], implementation [23:20], revision [7:4].[3:0]");
        }
        /* A device that answers all-ones is a dead function, not a chip id: report it as unreachable. */
        sim_mmio[0] = 0xffffffffu;
        sim_mmio[1] = 0xffffffffu;
        run_with(pair, 2);
        owner = gpu_scanout_device();
        expect(owner && owner->reg_state == 2, "an all-ones read is reported as not responding");
        {
            char text[4096];
            gpu_report(text, sizeof(text));
            expect(find_substring(text, "BAR0 mapped, but every read returned 0xffffffff") != NULL,
                   "a dead function is described as not answering, not as a chip id");
            expect(find_substring(text, "first dword 0xffffffff") == NULL,
                   "an unreachable device is never reported as if the all-ones value were an id");
        }
        /* QEMU's std VGA is the case this guard exists for: its BAR0 *is* the frame buffer the
         * kernel already maps for the console, so a second mapping would be a duplicate page-table
         * request for the same aperture.  Skipping it is the correct answer, not a missed probe. */
        {
            uint64_t keep2 = fake_fb.base;
            struct sim_device std[1] = {
                {0, 0, 0, 3, 0, 0, 0x1234, 0x1111, {0xe0000000u, 0u, 0u, 0u, 0u, 0u}, 7},
            };
            fake_fb.base = 0xe0000000ull;
            sim_maps = 0;
            run_with(std, 1);
            const struct gpu_device *vga = gpu_device(0);
            expect(vga && vga->reg_state == 5,
                   "a BAR0 that is the scanout aperture is reported as skipped, not as unprobed");
            expect(sim_maps == 0, "the scanout aperture is never mapped twice");
            fake_fb.base = keep2;
        }
        /* Memory decode off means the kernel must not go looking, because making it work would mean
         * writing configuration space, which detection never does. */
        pair[0].command = 0x00000006 & ~0x2u;
        run_with(pair, 2);
        owner = gpu_scanout_device();
        expect(owner && owner->reg_state == 4, "memory decode disabled is detected and reported as such");
        expect(sim_maps == 0, "no mapping was attempted while memory decoding was disabled");
        fake_fb.base = keep;
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
