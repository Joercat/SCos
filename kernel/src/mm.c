/*
 * SCos native - physical page allocator.
 *
 * Manages the region between the end of the kernel image and the top of
 * usable memory reported by the bootloader, as a simple first-fit free list
 * with 4 KB page granularity.
 */
#include "scos.h"

#define PAGE 4096

extern u32 _kernel_end;

struct block {
    struct block *next;
    u32 pages;
};

static struct block *free_list;
static u32 managed_base, managed_pages, free_pages;

void mm_init(void)
{
    u32 base = ((u32)&_kernel_end + PAGE - 1) & ~(PAGE - 1);
    u64 top = (u64)boot_info.mem_kb * 1024;
    if (top > 0x3F000000ULL) top = 0x3F000000ULL;   /* cap below 1 GB (VBE LFB) */
    u32 top32 = (u32)top & ~(PAGE - 1);
    if (top32 <= base + 4 * PAGE) {
        klog("mm: not enough memory (base %x top %x)", base, top32);
        for (;;) cpu_hlt();
    }
    managed_base = base;
    managed_pages = (top32 - base) / PAGE;
    free_pages = managed_pages;

    free_list = (struct block *)base;
    free_list->next = NULL;
    free_list->pages = managed_pages;
    klog("mm: %u KB managed (%u pages) at %x", managed_pages * 4, managed_pages, base);
}

void *palloc(u32 bytes)
{
    u32 pages = (bytes + PAGE - 1) / PAGE;
    if (!pages) pages = 1;
    irq_disable();
    struct block *b = free_list, *prev = NULL;
    while (b) {
        if (b->pages >= pages) {
            if (b->pages == pages) {
                if (prev) prev->next = b->next; else free_list = b->next;
            } else {
                struct block *rest = (struct block *)((u32)b + pages * PAGE);
                rest->next = b->next;
                rest->pages = b->pages - pages;
                if (prev) prev->next = rest; else free_list = rest;
                b->pages = pages;
            }
            free_pages -= pages;
            irq_enable();
            return b;
        }
        prev = b;
        b = b->next;
    }
    irq_enable();
    klog("mm: palloc(%u) FAILED", bytes);
    return NULL;
}

void pfree(void *p, u32 bytes)
{
    if (!p) return;
    u32 pages = (bytes + PAGE - 1) / PAGE;
    if (!pages) pages = 1;
    struct block *b = p;
    b->pages = pages;
    irq_disable();
    /* insert address-sorted and coalesce with neighbours so that repeated
     * window resize cycles cannot fragment the heap */
    struct block **pp = &free_list, *prev = NULL;
    while (*pp && *pp < b) { prev = *pp; pp = &(*pp)->next; }
    b->next = *pp;
    *pp = b;
    if (b->next && (u32)b->next == (u32)b + pages * PAGE) {
        b->pages += b->next->pages;
        b->next = b->next->next;
    }
    if (prev && (u32)prev + prev->pages * PAGE == (u32)b) {
        prev->pages += b->pages;
        prev->next = b->next;
    }
    free_pages += pages;
    irq_enable();
}

u32 mm_total_kb(void) { return managed_pages * 4; }
u32 mm_free_kb(void)  { return free_pages * 4; }

void mm_stats(u32 *total_kb, u32 *free_kb)
{
    *total_kb = mm_total_kb();
    *free_kb = mm_free_kb();
}
