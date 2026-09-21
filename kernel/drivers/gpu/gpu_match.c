/* Owner of the generated device tables.
 *
 * `gpu_ids.h` is produced by tools/research/gen_gpu_tables.py from the upstream
 * drivers' own binding tables at a pinned Haiku commit; it declares `static`
 * arrays, so exactly one translation unit may include it.  This is that unit, and
 * the accessors below are what the rest of the kernel sees.  Keeping the ownership
 * here means a regenerated table cannot silently change two files.
 */
#include <stddef.h>    /* NULL, offsetof: this TU is shared with the boot stub, which has no libc */
#include "gpu_match.h"
#include "gpu_ids.h"

int gpu_match_family_count(void)
{
    return (int)GPU_MATCH_COUNT;
}

const struct gpu_match *gpu_match_family(int index)
{
    if (index < 0 || index >= (int)GPU_MATCH_COUNT) return 0;
    return &gpu_match_table[index];
}

int gpu_match_id_total(void)
{
    return (int)GPU_ID_TOTAL;
}

/* The family's own class-code predicate, as generated from its source. */
static int class_matches(const struct gpu_match *match, uint8_t subclass)
{
    if (match->class_base != 0xff && match->class_base != 3) return 0;
    if (match->class_sub_a == 0xff) return 1;
    if (match->class_sub_a == subclass) return 1;
    if (match->class_sub_b != 0xff && match->class_sub_b == subclass) return 1;
    return 0;
}

/* Vendor lives in the row, not the family record: nvidia's driver pairs four vendors with four
 * lists (0x10de plus the ELSA, STB/SGS-Thompson and Varisys rebrands), so a single vendor per
 * family would drop the rebranded cards that upstream does bind. */
static const struct gpu_pci_id *match_id(const struct gpu_match *match, uint16_t vendor,
                                        uint16_t device)
{
    if (!match->ids || !match->id_count) return 0;
    for (unsigned i = 0; i < match->id_count; i++)
        if (match->ids[i].vendor == vendor && match->ids[i].device == device)
            return &match->ids[i];
    return 0;
}

/* Shared by the kernel's own scan and by the boot stub's module choice, which is the whole reason
 * this function is in a header-free TU both link against: one implementation, so the family the
 * firmware loaded a module for and the family the kernel detects cannot disagree by construction. */
const struct gpu_match *gpu_match_device(uint16_t vendor, uint16_t device, uint8_t subclass,
                                        const struct gpu_pci_id **row_out)
{
    if (row_out) *row_out = 0;
    for (int f = 0; f < (int)GPU_MATCH_COUNT; f++) {
        const struct gpu_match *match = &gpu_match_table[f];
        if (!match->id_count) continue;              /* no ID table to match against */
        if (!class_matches(match, subclass)) continue;
        const struct gpu_pci_id *row = match_id(match, vendor, device);
        if (!row) continue;
        if (row_out) *row_out = row;
        return match;
    }
    return 0;
}

const char *gpu_family_name(const struct gpu_match *match)
{
    return match ? match->family : 0;
}
