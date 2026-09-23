/* Header-only PCI config walk for the boot stub.  See pci_scan.h for why it is a separate unit.
 *
 * Both header tests below read byte 2 of the dword at 0x0c (that is register 0x0e, where PCI puts the
 * header type and the multifunction bit).  Reading the low byte of that dword instead - which is the cache
 * line size, and which the code this unit replaces did - finds a bridge on almost no machine and on no
 * machine reliably.
 *
 * The rules this unit implements, each one a place a real machine differs from the machine a test harness
 * has:
 *   - A class-01/02/80 display function can live on any bus, so buses reached through a type-1 header are
 *     walked, and *every* bus in [secondary, subordinate] is visited: a PCIe root port links exactly one
 *     bus, so secondary == subordinate there, and a loop that begins one bus above the secondary, or stops
 *     one bus below the subordinate, visits nothing at all.  That is the reason a discrete GPU behind a
 *     root port used to be missed while the same GPU in a QEMU guest was not.
 *   - Functions 1..7 are probed only when the header's multifunction bit is set.  Reading function 1 of a
 *     single-function device is not guaranteed to return all-ones: some bridges answer with the function-0
 *     id, and treating that alias as a second display function would name a device that is not there.
 *   - The walk counts what it inspected and what it had to drop, so the difference between "the bus was
 *     empty" and "the bus was never reached" survives into the report.
 *   - Reads only, always.  No BAR sizing, no command-register write, no reset.
 */
#include <stddef.h>
#include "pci_scan.h"

#define PCI_HEADER_TYPE 0x0cu
#define PCI_CLASS_REVISION 0x08u
#define PCI_VENDOR_DEVICE 0x00u
#define PCI_PRIMARY_BUSNUMBERS 0x18u    /* type-1: secondary, subordinate, at 0x19 and 0x1a */

struct walk {
    const struct pci_config_reader *io;
    struct pci_display_scan *out;
    uint8_t seen[256];
    unsigned visits;
};

static uint32_t rd(struct walk *w, uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    if (!w->io->read) return 0xffffffffu;
    return w->io->read(w->io->context, bus, slot, function, offset);
}

static int live(uint32_t vendev)
{
    return vendev != 0xffffffffu && vendev != 0u;
}

static void consider(struct walk *w, uint8_t bus, uint8_t slot, uint8_t function, uint32_t vendev,
                     uint32_t ccrev)
{
    uint32_t cls = ccrev >> 16;                 /* [31:24] base class, [23:16] subclass */
    w->out->functions_inspected++;
    if (((cls >> 8) & 0xffu) != 3u) return;     /* not a display controller */
    w->out->display_functions++;
    uint16_t vendor = (uint16_t)vendev, device = (uint16_t)(vendev >> 16);
    const struct gpu_match *match = gpu_match_device(vendor, device, (uint8_t)cls, 0);
    if (match && !w->out->match) {
        w->out->match = match;
        w->out->found = 1;
        w->out->bus = bus;
        w->out->slot = slot;
        w->out->function = function;
        w->out->subclass = (uint8_t)cls;        /* [23:16] of the class dword, as the matcher wants it */
        w->out->vendor = vendor;
        w->out->device = device;
    }
}

/* Walk one bus.  Returns 1 as soon as a match is held, because the stub stages exactly one module and a
 * second match would be a decision it must not make silently. */
static int walk_bus(struct walk *w, uint8_t bus, unsigned depth)
{
    if (depth > PCI_SCAN_MAX_DEPTH) return 0;
    w->out->buses_scanned++;
    for (uint8_t slot = 0; slot < 32u; slot++) {
        uint32_t vendev = rd(w, bus, slot, 0, PCI_VENDOR_DEVICE);
        if (!live(vendev)) continue;
        uint32_t header = rd(w, bus, slot, 0, PCI_HEADER_TYPE);
        uint32_t ccrev = rd(w, bus, slot, 0, PCI_CLASS_REVISION);
        consider(w, bus, slot, 0, vendev, ccrev);
        /* Only what the header claims: eight functions when bit 7 is set, one when it is not. */
        if (header & (1u << 23)) {        /* register 0x0e bit 7: the device itself says it has more */
            for (uint8_t function = 1; function < 8u; function++) {
                uint32_t other = rd(w, bus, slot, function, PCI_VENDOR_DEVICE);
                if (!live(other)) continue;
                consider(w, bus, slot, function, other,
                         rd(w, bus, slot, function, PCI_CLASS_REVISION));
            }
        }
        if (((header >> 16) & 0x7fu) != 1u) continue;   /* not a card bus: nothing below to see */
        uint32_t busnumbers = rd(w, bus, slot, 0, PCI_PRIMARY_BUSNUMBERS);
        unsigned secondary = (busnumbers >> 8) & 0xffu, subordinate = (busnumbers >> 16) & 0xffu;
        w->out->bridges_followed++;
        /* Inclusive at both ends, and bounded: a root port's own range is one bus wide, and a range that
         * names the bus we are standing on (a bridge firmware left half-configured) must not recurse. */
        for (unsigned next = secondary; next <= subordinate && next < 256u; next++) {
            if (!next || next == bus || w->seen[next]) continue;
            if (w->visits >= PCI_SCAN_MAX_BUS_VISITS) {
                /* Recorded rather than silently truncated: the report has to be able to say that the walk
                 * stopped for its own bound, not that the buses it stopped on were empty. */
                w->out->buses_not_visited_at_cap += (subordinate < 255u ? subordinate : 254u) - next + 1u;
                break;
            }
            w->seen[next] = 1u;
            w->visits++;
            if (walk_bus(w, (uint8_t)next, depth + 1u) && w->out->match) return 1;
        }
    }
    return w->out->match != 0;
}

int pci_scan_display_functions(const struct pci_config_reader *io, struct pci_display_scan *out)
{
    struct walk w;
    if (!io || !out) return 0;
    *out = (struct pci_display_scan){0};
    w.io = io;
    w.out = out;
    w.visits = 0;
    for (unsigned i = 0; i < sizeof(w.seen); i++) w.seen[i] = 0;
    w.seen[0] = 1u;                             /* the root bus, which the walk starts on */
    walk_bus(&w, 0, 0);
    return out->found;
}
