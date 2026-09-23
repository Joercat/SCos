/* Host harness for the boot stub's PCI display scan: the shipped walk, a synthetic config space.
 *
 * Why this unit is tested here and not only in QEMU: the whole reason it exists is a machine QEMU cannot
 * present - a GeForce RTX 5050 on bus 1 behind a PCIe root port whose secondary and subordinate bus numbers
 * are *equal*.  The walk that shipped before this file started its bus loop one above the secondary and
 * stopped one below the subordinate, so for exactly that shape it visited no bus at all: the stub staged no
 * module, the kernel declined to read an aperture "a module may claim", and the chip was read by nobody
 * while the panel blamed the driver tables.  QEMU's x86 machines put their display device on bus 0, which
 * is precisely why that bug survived every boot-level test.  So the bus ranges are exercised here, against
 * fixtures that describe the real topology, and a hang or a cap is measured rather than assumed.
 *
 * Build and run through tools/tests/test_pci_scan.py, which compiles pci_scan.c and gpu_match.c unchanged.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pci_scan.h"
#include "gpu_match.h"

#define BUSES 14u
#define SLOTS 32u
#define FUNCTIONS 8u

struct fn {
    uint32_t vendev, ccrev, header, busnumbers;
};

static struct fn config[BUSES][SLOTS][FUNCTIONS];
static unsigned failures, reads, checks, reads_outside_the_header;

static void check(int ok, const char *what)
{
    checks++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void reset(void)
{
    memset(config, 0, sizeof config);
    reads = 0;
}

static void put(unsigned bus, unsigned slot, unsigned function, uint32_t vendev, uint32_t ccrev,
                 uint32_t header, uint32_t busnumbers)
{
    config[bus][slot][function] = (struct fn){ vendev, ccrev, header, busnumbers };
}

/* Class dword as the device reports it: [31:24] base class, [23:16] subclass, [7:0] revision. */
#define CLASS(base, sub) (((uint32_t)(base) << 24) | ((uint32_t)(sub) << 16))
#define VNDEV(vendor, device) (((uint32_t)(device) << 16) | (uint32_t)(vendor))
#define HEADER_BRIDGE (1u << 16)
#define HEADER_MULTI (1u << 23)
#define BRIDGE_BUSES(secondary, subordinate) \
    (((uint32_t)(secondary) << 8) | ((uint32_t)(subordinate) << 16))

#define NVIDIA_5050 VNDEV(0x10de, 0x2d83)
#define CIRRUS_5446 VNDEV(0x1013, 0x00b8)
#define AUDIO_CLASS CLASS(0x04, 0x01)

/* The one accessor the walk is allowed: header reads, counted, all-ones where nothing is present.  Any
 * write would be visible in `config' being const-qualified here and would fail the build. */
static uint32_t read_config(void *context, uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    (void)context;
    reads++;
    if (bus >= BUSES || slot >= SLOTS || function >= FUNCTIONS) return 0xffffffffu;
    const struct fn *f = &config[bus][slot][function];
    if (!f->vendev) return 0xffffffffu;
    switch (offset) {
    case 0x00: return f->vendev;
    case 0x08: return f->ccrev;
    case 0x0c: return f->header;
    case 0x18: return f->busnumbers;
    default:
        /* Any offset outside the four header dwords would mean the walk is doing something a boot stub has
         * no business doing - sizing a BAR, poking a command register.  Counted, so the check below can
         * fail the run rather than trust the source to be read carefully. */
        reads_outside_the_header++;
        return 0u;
    }
}

static int scan(struct pci_display_scan *out)
{
    const struct pci_config_reader io = { read_config, 0 };
    return pci_scan_display_functions(&io, out);
}

int main(void)
{
    struct pci_display_scan s;

    /* ---- 1. the shape every QEMU guest has: the display function on bus 0. ---- */
    reset();
    put(0, 1, 0, CIRRUS_5446, CLASS(3, 0), 0, 0);
    check(scan(&s) == 1 && s.found && s.bus == 0 && s.slot == 1 && s.function == 0,
          "a display function on the root bus is found where a QEMU guest puts it");
    check(s.functions_inspected == 1 && s.display_functions == 1 && s.buses_scanned == 1 &&
          s.bridges_followed == 0, "and the counts describe one bus with one display function on it");
    check(reads == 34 && reads_outside_the_header == 0,
          "one vendor read per slot plus class and header for the live function, and no other offset at all");

    /* ---- 2. the shape a real laptop has: the GPU on bus 1, behind a root port with sec == sub. ---- */
    reset();
    put(0, 0, 0, VNDEV(0x8086, 0x09a0), CLASS(6, 4), 0, 0);            /* host bridge, not a display */
    put(0, 1, 0, VNDEV(0x8086, 0x01a1), AUDIO_CLASS, HEADER_BRIDGE, BRIDGE_BUSES(1, 1));
    put(1, 0, 0, NVIDIA_5050, CLASS(3, 0), 0, 0);
    check(scan(&s) == 1 && s.found && s.bus == 1 && s.slot == 0 && s.function == 0,
          "the GeForce RTX 5050 on the far side of a root port whose range is one bus wide is found");
    check(s.match && strcmp(s.match->family, "nvidia") == 0, "and it is the family the binding table claims");
    check(s.vendor == 0x10de && s.device == 0x2d83 && s.subclass == 0,
          "the id and subclass the stub hands to its module decision are the ones the device reported");
    check(!gpu_match_named_only(s.vendor, s.device, s.subclass),
          "and the id is not naming-only, so a module may be staged for it");
    check(s.buses_scanned == 2 && s.bridges_followed == 1 && s.display_functions == 1,
          "the bridge was followed to the one bus it links, and the host bridge was not");
    check(s.functions_inspected == 3,
          "every function that exists was inspected - the host bridge and the root port included, since "
          "knowing they were looked at is what makes \"the bus was empty\" a measured statement");

    /* ---- 3. the same walk over a window that spans several buses. ---- */
    reset();
    put(0, 1, 0, VNDEV(0x8086, 0x01a1), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(1, 3));
    put(3, 2, 0, NVIDIA_5050, CLASS(3, 0), 0, 0);
    check(scan(&s) == 1 && s.bus == 3, "a device at the far end of a three-bus window is found");
    check(s.buses_scanned == 4, "bus 0 plus every bus the window names, including both endpoints");

    /* ---- 4. firmware that describes a bridge badly must not hang or wander. ---- */
    reset();
    put(0, 1, 0, VNDEV(0x8086, 0x01a1), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(0, 255));
    put(1, 0, 0, VNDEV(0x8086, 0x01a2), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(1, 255));
    check(scan(&s) == 0, "a bridge that points at its own bus finds nothing and returns");
    check(s.buses_scanned <= PCI_SCAN_MAX_BUS_VISITS + 1u, "and the bound on bus visits held");
    check(s.buses_not_visited_at_cap > 0, "the buses the bound dropped are counted, not silently lost");
    check(reads < 200000u && reads_outside_the_header == 0,
          "a bounded number of header reads, and none of them outside the header");

    /* ---- 5. single-function devices: no alias may be taken for a second display function. ---- */
    reset();
    put(0, 1, 0, CIRRUS_5446, CLASS(3, 0), 0, 0);
    put(0, 1, 1, CIRRUS_5446, CLASS(3, 0), 0, 0);       /* a bridge that echoes function 0 */
    check(scan(&s) == 1 && s.function == 0 && s.display_functions == 1,
          "a function the header does not claim is not probed, so its alias is not counted twice");

    /* ---- 6. a genuine multifunction device, display on the second function. ---- */
    reset();
    put(0, 2, 0, VNDEV(0x10de, 0x0fb9), AUDIO_CLASS, HEADER_MULTI, 0);
    put(0, 2, 1, NVIDIA_5050, CLASS(3, 0), HEADER_MULTI, 0);
    check(scan(&s) == 1 && s.slot == 2 && s.function == 1,
          "all eight functions of a multifunction device are examined, and a later one may be the GPU");
    check(s.display_functions == 1, "the audio function is seen but not counted as display");

    /* ---- 7. the depth bound, at the boundary and one past it. ---- */
    reset();
    for (unsigned b = 0; b + 1 < PCI_SCAN_MAX_DEPTH + 2u; b++)
        put(b, 1, 0, VNDEV(0x8086, 0x01a1), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(b + 1, b + 1));
    put(PCI_SCAN_MAX_DEPTH + 1u, 0, 0, NVIDIA_5050, CLASS(3, 0), 0, 0);
    check(scan(&s) == 0, "a device one bus past the depth bound is not reached, and that is the rule");
    check(s.buses_scanned == PCI_SCAN_MAX_DEPTH + 1u,
          "the walk reports how deep it did go, and the bus it refused is not counted as seen");
    reset();
    for (unsigned b = 0; b < PCI_SCAN_MAX_DEPTH; b++)
        put(b, 1, 0, VNDEV(0x8086, 0x01a1), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(b + 1, b + 1));
    put(PCI_SCAN_MAX_DEPTH, 0, 0, NVIDIA_5050, CLASS(3, 0), 0, 0);
    check(scan(&s) == 1 && s.bus == PCI_SCAN_MAX_DEPTH, "at the bound itself it is still found");

    /* ---- 8. a bus with nothing on it, which is a different answer from an unreached one. ---- */
    reset();
    check(scan(&s) == 0 && s.functions_inspected == 0 && s.buses_scanned == 1 &&
          s.display_functions == 0,
          "an empty root bus reports zero functions on one bus, and no module was owed to anything");

    /* ---- 9. the class rule: the same id at a non-display class is not a display function. ---- */
    reset();
    put(0, 1, 0, NVIDIA_5050, CLASS(6, 4), 0, 0);
    check(scan(&s) == 0 && s.display_functions == 0,
          "a function outside base class 3 is not claimed, whatever its id says");

    /* ---- 10. two GPUs: the first match wins, because the stub stages exactly one module. ---- */
    reset();
    put(0, 1, 0, CIRRUS_5446, CLASS(3, 0), 0, 0);
    put(1, 0, 0, NVIDIA_5050, CLASS(3, 0), 0, 0);
    put(0, 31, 0, VNDEV(0x8086, 0x01a1), CLASS(6, 4), HEADER_BRIDGE, BRIDGE_BUSES(1, 1));
    check(scan(&s) == 1 && s.device == 0x00b8 && s.display_functions == 2,
          "with two display functions the lowest address wins and both are counted, so the report can say so");

    check(reads_outside_the_header == 0, "no scenario ever made the walk read past the configuration header");
    printf("%s: boot-stub PCI display scan, %u check(s), %u failure(s)\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures != 0;
}
