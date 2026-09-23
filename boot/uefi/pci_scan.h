/* The boot stub's read-only PCI display scan, kept in its own unit so the walk can be run against a
 * fixture config space instead of only against firmware.
 *
 * Why this file exists at all: on a machine whose GPU sits behind a PCIe root port, the display function is
 * on a secondary bus, and a scan that visits bus 0 alone never sees it.  The stub then stages no driver
 * module, the kernel declines to read an aperture a module might claim, and the chip ends up unread by
 * anybody - which is a real machine's outcome, not a hypothetical one.  The bounds below are the difference
 * between "walks the hierarchy" and "hangs on a firmware that describes a bridge badly", so they are tested
 * explicitly in tools/tests/pci_scan_host.c rather than trusted.
 */
#ifndef SCOS_UEFI_PCI_SCAN_H
#define SCOS_UEFI_PCI_SCAN_H
#include <stdint.h>
#include "gpu_match.h"

/* 256 buses exist in a PCI express configuration space; a real hierarchy is far shallower than eight
 * bridges, and a subordinate of 0xff (which firmware sometimes leaves in place) is capped rather than
 * followed to the end of the address space. */
#define PCI_SCAN_MAX_DEPTH 8u
#define PCI_SCAN_MAX_BUS_VISITS 64u

/* Config reads go through the caller, because who may read configuration space differs per owner: the stub
 * uses the 0xcf8/0xcfc index ports, the kernel has its own accessor, and a test has an array. */
struct pci_config_reader {
    uint32_t (*read)(void *context, uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
    void *context;
};

struct pci_display_scan {
    int found;                                /* a class-0 function that the binding table claims */
    uint8_t bus, slot, function, subclass;    /* where it is */
    uint16_t vendor, device;
    const struct gpu_match *match;

    /* What the walk cost, and what it refused to do.  These are not diagnostics for their own sake: a
     * report that says "no module staged" cannot distinguish a disk without the file from a bus that was
     * never looked at, and those two want different fixes. */
    unsigned functions_inspected;
    unsigned buses_scanned;
    unsigned display_functions;
    unsigned bridges_followed;                /* type-1 headers whose bus ranges were walked */
    unsigned buses_not_visited_at_cap;        /* secondary/subordinate buses dropped by the bound above */
};

/* Returns 1 when a display function matched a family, 0 otherwise; `out` is filled either way.  Nothing
 * here writes to configuration space, sizes a BAR, resets a device or reads a BAR - it is header reads
 * only, and the caller decides what any of it means. */
int pci_scan_display_functions(const struct pci_config_reader *io, struct pci_display_scan *out);

#endif
