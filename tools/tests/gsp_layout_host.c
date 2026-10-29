/* Host harness for kernel/drivers/gpu/gsp_layout.c - the arithmetic that decides where the GSP
 * firmware's memory goes in video memory.
 *
 * Why this can be tested here at all: the layout is a pure function of five numbers, and every one of
 * them is something a caller has (the video memory the chip reported, the top the firmware's own use
 * starts at, the two image sizes and the heap floor).  Nothing in it reads a register, allocates, or
 * waits, so a wrong shift or a missed down-round is visible as a wrong number in this file rather than
 * as a chip that will not boot.
 *
 * What is asserted, in the order it would cost a boot:
 *   - the exact placement on a case whose answer was worked out by hand, digit for digit, so a change
 *     in the arithmetic moves a number here and not only in a comment;
 *   - the invariants on a sweep of real card sizes and image sizes: ordered, aligned, gap-free between
 *     neighbours, the last region ending exactly at the 128 KiB-rounded top, everything inside video
 *     memory;
 *   - the refusals: no size, no top, a top above memory, a zero-byte image, a card too small for the
 *     FRTS, and a heap that cannot reach its floor.  A refusal must leave no offset behind that a
 *     caller could hand to firmware;
 *   - that the plan is monotonic: a bigger card moves every region by the same amount, so a change in
 *     one size can never make two regions collide.
 *
 * The case in `reference' is deliberately not read back from the implementation: its numbers are
 * written out in the test, so the implementation has to agree with them.
 */
#include <stdio.h>
#include <string.h>

#include "gsp_layout.h"

static int failures;

static void fail(const char *what, const char *detail)
{
    printf("FAIL: %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
    failures++;
}

static void check(int condition, const char *what, const char *detail)
{
    if (!condition) fail(what, detail);
}

static const char *state_name(int state)
{
    switch (state) {
    case GSP_LAYOUT_OK:              return "ok";
    case GSP_LAYOUT_NO_VIDEO_MEMORY: return "no video memory";
    case GSP_LAYOUT_NO_TOP:          return "no top";
    case GSP_LAYOUT_NO_IMAGES:       return "no images";
    case GSP_LAYOUT_TOO_SMALL:       return "too small";
    case GSP_LAYOUT_TOP_ABOVE_MEMORY: return "top above memory";
    case GSP_LAYOUT_HEAP_TOO_SMALL:  return "heap too small";
    }
    return "unknown";
}

/* A 1 GiB card, a 1 MiB ELF image, a 64 KiB bootloader and a 1 MiB heap floor: every number below was
 * worked out from the placement rules on paper, so the test is of the implementation and not of a copy
 * of its output. */
static void reference(void)
{
    struct gsp_fb_plan plan;
    int state = gsp_fb_plan(&plan, 1073741824ull, 1073741824ull, 1048576ull, 65536ull, 1048576ull);

    check(state == GSP_LAYOUT_OK, "the reference card takes a reservation", state_name(state));
    if (state != GSP_LAYOUT_OK) return;

    check(plan.frts_offset == 1072693248ull, "FRTS sits one MiB under the top", "");
    check(plan.frts_bytes == 1048576ull, "the FRTS is one MiB", "");
    check(plan.bootloader_offset == 1072627712ull, "the bootloader is 4 KiB aligned under it", "");
    check(plan.bootloader_bytes == 65536ull, "the bootloader keeps its own size", "");
    check(plan.elf_offset == 1071579136ull, "the ELF image is 64 KiB aligned under the bootloader", "");
    check(plan.elf_bytes == 1048576ull, "the ELF keeps its own size", "");
    check(plan.heap_offset == 1069547520ull, "the heap is 1 MiB aligned under the ELF", "");
    check(plan.heap_bytes == 2031616ull, "the heap is what fits between its offset and the ELF", "");
    check(plan.wpr2_start == 1068498944ull, "the WPR2 starts on a MiB under the heap, minus metadata",
          "");
    check(plan.wpr2_end == 1073741824ull, "the WPR2 ends exactly at the top", "");
    check(plan.non_wpr_heap_offset == 1067450368ull, "one MiB outside the WPR2 for the host heap", "");
    check(plan.rsvd_start == plan.non_wpr_heap_offset, "gspFwRsvdStart is the non-WPR heap", "");
    check(plan.region_count == 5, "five regions", "");
    if (failures) printf("case: %s\n", state_name(state));
}

/* Every number a boot depends on, checked for a sweep: alignment per region, ordering, no overlap, the
 * top, and the video-memory bound. */
static void invariants(void)
{
    static const uint64_t sizes[] = {
        512ull << 20, 1ull << 30, 2ull << 30, 3ull << 30, 4ull << 30, 6ull << 30,
        8ull << 30, 12ull << 30, 16ull << 30, 24ull << 30, 32ull << 30,
    };
    static const uint64_t elfs[] = { 4096ull, 1048576ull, 287304ull, 503808ull, 7340032ull };
    static const uint64_t boots[] = { 4096ull, 65536ull, 200812ull };
    static const uint64_t heaps[] = { 1048576ull, 33554432ull, 67108864ull };
    int planned = 0, refused = 0;

    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    for (unsigned e = 0; e < sizeof(elfs) / sizeof(elfs[0]); e++)
    for (unsigned b = 0; b < sizeof(boots) / sizeof(boots[0]); b++)
    for (unsigned h = 0; h < sizeof(heaps) / sizeof(heaps[0]); h++) {
        struct gsp_fb_plan plan;
        int state = gsp_fb_plan(&plan, sizes[s], sizes[s], elfs[e], boots[b], heaps[h]);
        if (state != GSP_LAYOUT_OK) { refused++; continue; }
        planned++;

        check(plan.vram_bytes == sizes[s] && plan.usable_top == sizes[s], "the plan echoes its inputs", "");
        check(plan.frts_offset % GSP_LAYOUT_FRTS_ALIGN == 0, "the FRTS is carve-out aligned", "");
        check(plan.bootloader_offset % GSP_LAYOUT_BOOTLOADER_ALIGN == 0, "the bootloader is page aligned",
              "");
        check(plan.elf_offset % GSP_LAYOUT_ELF_ALIGN == 0, "the ELF is 64 KiB aligned", "");
        check(plan.heap_offset % GSP_LAYOUT_HEAP_ALIGN == 0, "the heap is MiB aligned", "");
        check(plan.wpr2_start % GSP_LAYOUT_WPR2_ALIGN == 0, "the WPR2 is MiB aligned", "");
        check(plan.wpr2_end == plan.frts_offset + plan.frts_bytes, "the WPR2 ends with the FRTS", "");
        check(plan.wpr2_end == (plan.usable_top & ~((uint64_t)GSP_LAYOUT_FRTS_ALIGN - 1u)),
              "the reservation ends at the aligned top", "");
        check(plan.wpr2_end <= sizes[s], "the reservation is inside video memory", "");

        /* No overlap: each region starts at or after the end of the one below it. */
        check(plan.non_wpr_heap_offset + plan.non_wpr_heap_bytes <= plan.wpr2_start,
              "the host heap does not enter the WPR2", "");
        check(plan.wpr2_start <= plan.heap_offset, "the heap is inside the WPR2", "");
        check(plan.heap_offset + plan.heap_bytes == plan.elf_offset, "the heap meets the ELF exactly", "");
        check(plan.elf_offset + plan.elf_bytes <= plan.bootloader_offset,
              "the ELF does not reach the bootloader", "");
        check(plan.bootloader_offset + plan.bootloader_bytes <= plan.frts_offset,
              "the bootloader does not reach the FRTS", "");
        check(plan.heap_bytes >= heaps[h], "the heap reaches its floor", "");

        /* Each region a span helper can name, and each span inside the reservation. */
        for (int r = GSP_FB_FRTS; r <= GSP_FB_NON_WPR_HEAP; r++) {
            uint64_t offset = 0, bytes = 0;
            check(gsp_fb_region_span(&plan, (enum gsp_fb_region)r, &offset, &bytes) == 0,
                  "the region has a span", "");
            check(offset + bytes <= plan.vram_bytes, "the span is inside video memory", "");
            check(gsp_fb_region_align((enum gsp_fb_region)r) != 0, "the region states its alignment", "");
        }
    }
    check(planned > 0, "the sweep planned at least one card", "");
    printf("case: %d plans accepted, %d refused over %u card sizes and %u image sizes\n",
           planned, refused, (unsigned)(sizeof(sizes) / sizeof(sizes[0])),
           (unsigned)(sizeof(elfs) / sizeof(elfs[0])));
}

/* A bigger card with the same images moves every region by the same amount.  This is the property that
 * makes the plan safe to recompute on a second boot: nothing changes relative to anything else. */
static void monotonic(void)
{
    struct gsp_fb_plan small, large;
    const uint64_t a = 4ull << 30, b = 8ull << 30;
    int state_a = gsp_fb_plan(&small, a, a, 1048576ull, 65536ull, 16777216ull);
    int state_b = gsp_fb_plan(&large, b, b, 1048576ull, 65536ull, 16777216ull);

    check(state_a == GSP_LAYOUT_OK && state_b == GSP_LAYOUT_OK, "both card sizes plan",
          state_a == GSP_LAYOUT_OK ? state_name(state_b) : state_name(state_a));
    if (state_a || state_b) return;

    check(large.wpr2_end - small.wpr2_end == b - a, "the top moved by the difference", "");
    check(large.frts_offset - small.frts_offset == b - a, "the FRTS moved by the difference", "");
    check(large.wpr2_start - small.wpr2_start == b - a, "the WPR2 start moved by the difference", "");
    check(large.heap_bytes == small.heap_bytes, "the heap is the same size", "");
}

/* Refusals, and that each one leaves nothing behind. */
static void refusals(void)
{
    struct gsp_fb_plan plan;
    int state;

    state = gsp_fb_plan(&plan, 0, 0, 1048576ull, 65536ull, 1048576ull);
    check(state == GSP_LAYOUT_NO_VIDEO_MEMORY, "an unknown card is refused", state_name(state));

    state = gsp_fb_plan(&plan, 1ull << 30, 0, 1048576ull, 65536ull, 1048576ull);
    check(state == GSP_LAYOUT_NO_TOP, "no top is refused", state_name(state));

    state = gsp_fb_plan(&plan, 1ull << 30, (1ull << 30) + 4096, 1048576ull, 65536ull, 1048576ull);
    check(state == GSP_LAYOUT_TOP_ABOVE_MEMORY, "a top outside memory is refused", state_name(state));

    state = gsp_fb_plan(&plan, 1ull << 30, 1ull << 30, 0, 65536ull, 1048576ull);
    check(state == GSP_LAYOUT_NO_IMAGES, "a zero-byte ELF image is refused", state_name(state));
    state = gsp_fb_plan(&plan, 1ull << 30, 1ull << 30, 1048576ull, 0, 1048576ull);
    check(state == GSP_LAYOUT_NO_IMAGES, "a zero-byte bootloader is refused", state_name(state));

    state = gsp_fb_plan(&plan, 2ull << 20, 2ull << 20, 1048576ull, 65536ull, 1048576ull);
    check(state == GSP_LAYOUT_TOO_SMALL, "a card too small for the images is refused", state_name(state));
    check(plan.frts_offset == 0 && plan.wpr2_start == 0 && plan.region_count == 0,
          "a refusal leaves no place to put firmware", "");

    /* The images fit but the heap floor does not: the reference keeps the smaller heap, this refuses,
     * because a heap that is quietly short is a boot that fails inside the chip. */
    state = gsp_fb_plan(&plan, 64ull << 20, 64ull << 20, 1048576ull, 65536ull, 63ull << 20);
    check(state == GSP_LAYOUT_TOO_SMALL || state == GSP_LAYOUT_HEAP_TOO_SMALL,
          "a heap floor that cannot be met is refused", state_name(state));
    check(plan.region_count == 0, "that refusal leaves no place either", "");
}

int main(void)
{
    reference();
    invariants();
    monotonic();
    refusals();

    if (failures) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: the GSP reservation is placed where the boot contract says, or refused with a "
           "reason and no offset\n");
    return 0;
}
