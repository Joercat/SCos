/*
 * Where the GSP firmware's memory has to go in video memory, and why every byte of it is decided by
 * arithmetic rather than by a preference.
 *
 * What this is for.  On Turing and later the coprocessor is booted by the FSP from an FMC image the
 * host places in *system* memory, but everything it does afterwards lives in a reserved region of
 * *video* memory - the "WPR2" - that the host has to describe before the handover, because the FMC
 * reads the description back out of it and fails with FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR if the
 * numbers do not line up.  Three things must therefore be true, and they are what this file computes:
 *
 *   1. the frame buffer's top is reserved *backwards*, from the FRTS region down, so every offset is
 *      an offset from the end of memory and nothing depends on where the usable part starts;
 *   2. each region is aligned to what its own consumer demands (128 KiB for the FRTS because the
 *      carve-out is described in that granularity, 4 KiB for the bootloader image, 64 KiB for the
 *      ELF image the FMC walks, 1 MiB for the heaps and for the WPR2's own start);
 *   3. nothing overlaps, and the whole thing fits below the frame buffer's top.
 *
 * Provenance, stated because the arithmetic is somebody else's definition and not an inference of
 * this tree's: the layout below is the sequence of placements in nouveau's
 * `tu102_gsp_oneinit()` (drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c) and the field list of
 * `GspFwWprMeta` filled by `gh100_gsp_wpr_meta_init()` (same directory, gh100.c).  nvkm is MIT
 * licensed; those two functions were read on 2026-10-28 to establish the order, the alignments and
 * the sizes.  The code here is written against this tree's own needs - a freestanding function with
 * no allocation, no device access and explicit refusals - and no line of it is copied: what is
 * reproduced is a layout, which is a fact about the chip's boot contract, not an implementation.
 *
 * What it deliberately does not do: allocate anything, read a register, or decide that a card has
 * enough memory.  Its answer for a card that cannot hold the reservation is a refusal with a named
 * reason, because a plan that silently shifts the FRTS down would hand the firmware the addresses it
 * would have had if the card were bigger - the class of bug that only shows up as a chip that will
 * not boot, with no message anywhere.
 */
#ifndef SCOS_GSP_LAYOUT_H
#define SCOS_GSP_LAYOUT_H

#include <stdint.h>

/* Alignments and sizes the boot contract fixes.  The names carry their consumers so a reader can see
 * which one is which without opening another file. */
#define GSP_LAYOUT_FRTS_BYTES          (1024u * 1024u)  /* the region FWSEC-FRTS writes into    */
#define GSP_LAYOUT_WPR2_ALIGN          (1024u * 1024u)  /* the WPR2's start                    */
#define GSP_LAYOUT_FRTS_ALIGN          (128u * 1024u)   /* the carve-out granularity           */
#define GSP_LAYOUT_BOOTLOADER_ALIGN    4096u            /* the bootloader image                */
#define GSP_LAYOUT_ELF_ALIGN           (64u * 1024u)    /* the ELF image the FMC walks         */
#define GSP_LAYOUT_HEAP_ALIGN          (1024u * 1024u)  /* the firmware heaps                  */
#define GSP_LAYOUT_NON_WPR_HEAP_BYTES  (1024u * 1024u)  /* outside the WPR, RW by the host     */
#define GSP_LAYOUT_META_BYTES          256u             /* sizeof(GspFwWprMeta), checked below */

/* Why a plan was refused.  A caller reports the reason; none of them is "the card is unsupported". */
enum gsp_layout_refusal {
    GSP_LAYOUT_OK = 0,
    GSP_LAYOUT_NO_VIDEO_MEMORY = 1,   /* the size is not known, so no offset from the end exists  */
    GSP_LAYOUT_NO_TOP = 2,            /* no usable top: nothing to place the FRTS below           */
    GSP_LAYOUT_NO_IMAGES = 3,         /* an image of zero bytes is not an image                   */
    GSP_LAYOUT_TOO_SMALL = 4,         /* the regions do not fit under the top                     */
    GSP_LAYOUT_TOP_ABOVE_MEMORY = 5,  /* the top is outside video memory                           */
    GSP_LAYOUT_HEAP_TOO_SMALL = 6     /* the heap that fits is smaller than the firmware needs    */
};

/* One reservation, every number an offset in bytes from the start of video memory - the unit the
 * WPR2 metadata itself uses. */
struct gsp_fb_plan {
    uint64_t vram_bytes;              /* input, echoed: what the chip reported                  */
    uint64_t usable_top;              /* input, echoed: where the firmware's own area begins     */

    uint64_t frts_offset, frts_bytes;
    uint64_t bootloader_offset, bootloader_bytes;
    uint64_t elf_offset, elf_bytes;
    uint64_t heap_offset, heap_bytes;         /* inside the WPR2                                */
    uint64_t wpr2_start, wpr2_end;            /* the reserved region itself                     */
    uint64_t non_wpr_heap_offset, non_wpr_heap_bytes;
    uint64_t rsvd_start;                      /* gspFwRsvdStart: the non-WPR heap's address      */

    uint32_t region_count;                    /* how many of the five regions are non-empty      */
};

/* Fill `plan' for one card.  `usable_top' is the address the firmware's own use of video memory starts
 * at (on a booted card the top of the VGA workspace, or the frame buffer's top when nothing reserved
 * anything); `elf_bytes' is the ELF image the FMC walks, `bootloader_bytes' the bootloader it runs
 * first, and `heap_min_bytes' the smallest firmware heap the caller will accept.  Returns
 * GSP_LAYOUT_OK or a refusal, and on refusal writes nothing that a caller could mistake for a place
 * to put firmware: every offset in `plan' is zeroed first. */
int gsp_fb_plan(struct gsp_fb_plan *plan, uint64_t vram_bytes, uint64_t usable_top,
                uint64_t elf_bytes, uint64_t bootloader_bytes, uint64_t heap_min_bytes);

/* The alignment the layout uses, for a caller that wants to state it in a report without repeating a
 * constant: returns 0 for an unknown region id. */
enum gsp_fb_region {
    GSP_FB_FRTS = 0, GSP_FB_BOOTLOADER = 1, GSP_FB_ELF = 2, GSP_FB_HEAP = 3,
    GSP_FB_WPR2 = 4, GSP_FB_NON_WPR_HEAP = 5
};
uint64_t gsp_fb_region_align(enum gsp_fb_region region);

/* The region's [offset, offset+bytes) for a plan, 0 if the region id is unknown. */
int gsp_fb_region_span(const struct gsp_fb_plan *plan, enum gsp_fb_region region,
                       uint64_t *offset, uint64_t *bytes);

#endif
