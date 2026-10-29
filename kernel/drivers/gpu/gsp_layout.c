/*
 * The GSP frame-buffer layout, as arithmetic and nothing else.  See kernel/include/gsp_layout.h for
 * what each region is for and where the sequence comes from; what is here is the translation of that
 * sequence into a function that either answers with a place for every region or refuses and says why.
 *
 * Two properties are worth stating because they are what the host test measures.  The plan is
 * *monotonic*: every offset is derived from the one above it, so a change in the video-memory size
 * moves the whole reservation and moves no region relative to another.  And the plan is *complete*:
 * the regions tile [non_wpr_heap_offset, frts_offset + frts_bytes) with no gap that a placement rule
 * did not create and no overlap at all - the firmware walks from one to the next by these numbers, so
 * an overlap is a region of memory two consumers both think they own.
 */
#include "gsp_layout.h"

/* Round `value' down to `align', both powers of two.  A down-round of a value smaller than the
 * alignment is zero, which the callers below treat as "this does not fit" rather than as offset 0. */
static uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1u);
}

int gsp_fb_plan(struct gsp_fb_plan *plan, uint64_t vram_bytes, uint64_t usable_top,
                uint64_t elf_bytes, uint64_t bootloader_bytes, uint64_t heap_min_bytes)
{
    uint64_t top, frts, boot, elf, heap, heap_want, wpr2, non_wpr;

    if (!plan) return GSP_LAYOUT_NO_TOP;
    *plan = (struct gsp_fb_plan){ 0 };
    plan->vram_bytes = vram_bytes;
    plan->usable_top = usable_top;

    if (!vram_bytes) return GSP_LAYOUT_NO_VIDEO_MEMORY;
    if (!usable_top) return GSP_LAYOUT_NO_TOP;
    if (usable_top > vram_bytes) return GSP_LAYOUT_TOP_ABOVE_MEMORY;
    if (!elf_bytes || !bootloader_bytes) return GSP_LAYOUT_NO_IMAGES;

    /* From the top down, in the order the firmware expects to find them: the FRTS first, because it
     * is the one region described in carve-out granularity rather than page granularity. */
    top = align_down(usable_top, GSP_LAYOUT_FRTS_ALIGN);
    if (top <= GSP_LAYOUT_FRTS_BYTES) return GSP_LAYOUT_TOO_SMALL;
    frts = top - GSP_LAYOUT_FRTS_BYTES;

    if (frts <= bootloader_bytes) return GSP_LAYOUT_TOO_SMALL;
    boot = align_down(frts - bootloader_bytes, GSP_LAYOUT_BOOTLOADER_ALIGN);

    if (boot <= elf_bytes) return GSP_LAYOUT_TOO_SMALL;
    elf = align_down(boot - elf_bytes, GSP_LAYOUT_ELF_ALIGN);

    /* The heap is placed by the size the firmware asks for, then *measured* against what fits between
     * itself and the ELF image.  The reference does the same and keeps the smaller of the two, which
     * is why a plan whose heap came out under the minimum is a refusal here: a firmware heap that is
     * quietly too small is a boot that fails inside the chip. */
    heap_want = heap_min_bytes ? heap_min_bytes : GSP_LAYOUT_HEAP_ALIGN;
    if (elf <= heap_want) return GSP_LAYOUT_TOO_SMALL;
    heap = align_down(elf - heap_want, GSP_LAYOUT_HEAP_ALIGN);
    if (heap < heap_want) return GSP_LAYOUT_HEAP_TOO_SMALL;

    wpr2 = heap > GSP_LAYOUT_META_BYTES ? align_down(heap - GSP_LAYOUT_META_BYTES, GSP_LAYOUT_WPR2_ALIGN)
                                        : 0;
    if (!wpr2) return GSP_LAYOUT_TOO_SMALL;

    non_wpr = wpr2 > GSP_LAYOUT_NON_WPR_HEAP_BYTES ? wpr2 - GSP_LAYOUT_NON_WPR_HEAP_BYTES : 0;
    if (!non_wpr) return GSP_LAYOUT_TOO_SMALL;

    plan->frts_offset = frts;
    plan->frts_bytes = GSP_LAYOUT_FRTS_BYTES;
    plan->bootloader_offset = boot;
    plan->bootloader_bytes = bootloader_bytes;
    plan->elf_offset = elf;
    plan->elf_bytes = elf_bytes;
    plan->heap_offset = heap;
    plan->heap_bytes = elf - heap;
    plan->wpr2_start = wpr2;
    plan->wpr2_end = frts + GSP_LAYOUT_FRTS_BYTES;
    plan->non_wpr_heap_offset = non_wpr;
    plan->non_wpr_heap_bytes = GSP_LAYOUT_NON_WPR_HEAP_BYTES;
    plan->rsvd_start = non_wpr;
    plan->region_count = 5;

    /* The invariant worth checking after the fact rather than trusting the derivation: the regions
     * are ordered the way the firmware walks them, the last one ends at the top, and all of it lies
     * under the video memory the chip reported. */
    int ordered = plan->non_wpr_heap_offset + plan->non_wpr_heap_bytes == plan->wpr2_start &&
                  plan->wpr2_start <= plan->heap_offset &&
                  plan->heap_offset + plan->heap_bytes == plan->elf_offset &&
                  plan->elf_offset + plan->elf_bytes <= plan->bootloader_offset &&
                  plan->bootloader_offset + plan->bootloader_bytes <= plan->frts_offset &&
                  plan->frts_offset + plan->frts_bytes == plan->wpr2_end &&
                  plan->wpr2_end <= vram_bytes;
    if (!ordered) {
        *plan = (struct gsp_fb_plan){ 0 };
        plan->vram_bytes = vram_bytes;
        plan->usable_top = usable_top;
        return GSP_LAYOUT_TOO_SMALL;
    }
    return GSP_LAYOUT_OK;
}

uint64_t gsp_fb_region_align(enum gsp_fb_region region)
{
    switch (region) {
    case GSP_FB_FRTS:         return GSP_LAYOUT_FRTS_ALIGN;
    case GSP_FB_BOOTLOADER:   return GSP_LAYOUT_BOOTLOADER_ALIGN;
    case GSP_FB_ELF:          return GSP_LAYOUT_ELF_ALIGN;
    case GSP_FB_HEAP:         return GSP_LAYOUT_HEAP_ALIGN;
    case GSP_FB_WPR2:         return GSP_LAYOUT_WPR2_ALIGN;
    case GSP_FB_NON_WPR_HEAP: return GSP_LAYOUT_HEAP_ALIGN;
    }
    return 0;
}

int gsp_fb_region_span(const struct gsp_fb_plan *plan, enum gsp_fb_region region,
                       uint64_t *offset, uint64_t *bytes)
{
    uint64_t o, b;

    if (!plan || !plan->region_count || !offset || !bytes) return -1;
    switch (region) {
    case GSP_FB_FRTS:         o = plan->frts_offset;         b = plan->frts_bytes;         break;
    case GSP_FB_BOOTLOADER:   o = plan->bootloader_offset;   b = plan->bootloader_bytes;   break;
    case GSP_FB_ELF:          o = plan->elf_offset;          b = plan->elf_bytes;          break;
    case GSP_FB_HEAP:         o = plan->heap_offset;         b = plan->heap_bytes;         break;
    case GSP_FB_WPR2:         o = plan->wpr2_start;          b = plan->wpr2_end - plan->wpr2_start; break;
    case GSP_FB_NON_WPR_HEAP: o = plan->non_wpr_heap_offset; b = plan->non_wpr_heap_bytes; break;
    default:                  return -1;
    }
    *offset = o;
    *bytes = b;
    return 0;
}
