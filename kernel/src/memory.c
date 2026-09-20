#include "kernel.h"
/* Boot-only, single-CPU identity-mapped page pool. This is deliberately NOT a
 * general heap, DMA allocator, high-memory allocator or per-process VM manager.
 * memory_init runs once with maskable interrupts off, after handoff validation.
 * BSS owns all page tables; the entire linked kernel span stays reserved.
 * Allocation/release serialize against ordinary IRQs, not NMI or other CPUs.
 * NMI/machine-check/panic handlers must never allocate or free pages here. */
#define PAGE UINT64_C(4096)
#define NX (UINT64_C(1)<<63)
#define COUNT (BOOT_IDENTITY_LIMIT/PAGE)
static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t directory[512] __attribute__((aligned(4096)));
static uint64_t tables[8][512] __attribute__((aligned(4096)));
_Static_assert(COUNT == sizeof(tables)/sizeof(uint64_t), "page-table coverage mismatch");
static uint8_t state[COUNT]; /* 0 reserved, 1 free, 2 allocated; not a fake heap */
static uint64_t free_pages;
static uint64_t irq_save(void) {uint64_t f;__asm__ volatile("pushfq;popq %0;cli":"=r"(f)::"memory");return f;}
static void irq_restore(uint64_t f) {if(f&512)__asm__ volatile("sti":::"memory");}
static int guard(uintptr_t p) {
    return p==(uintptr_t)stack_guard || p==(uintptr_t)df_guard ||
           p==(uintptr_t)nmi_guard || p==(uintptr_t)mc_guard;
}
void memory_init(const struct boot_handoff *b,const struct boot_map_entry *map) {
    if(!boot_range_usable(map,b->map_count,b->kernel_start,b->kernel_end-b->kernel_start))
        panic("kernel intersects non-usable E820 memory");
    pml4[0]=(uintptr_t)pdpt|3;
    pdpt[0]=(uintptr_t)directory|3;
    for(size_t i=0;i<8;i++) directory[i]=(uintptr_t)tables[i]|3;
    for(uintptr_t p=0;p<BOOT_IDENTITY_LIMIT;p+=PAGE) {
        uint64_t bits=0;
        if(p>=b->kernel_start && p<b->kernel_end && !guard(p)) {
            bits=1|NX;
            if(p>=(uintptr_t)_text_start && p<(uintptr_t)_text_end) bits=1;
            else if(!(p>=(uintptr_t)_rodata_start && p<(uintptr_t)_rodata_end)) bits|=2;
        } else if(p>=b->kernel_end && boot_range_usable(map,b->map_count,p,PAGE)) {
            bits=3|NX;state[p/PAGE]=1;free_pages++;
        }
        if(p==0xb8000) bits=3|NX|0x18; /* VGA device memory: UC, never executable */
        if(bits) tables[p/(PAGE*512)][(p/PAGE)%512]=p|bits;
    }
    if(!free_pages) panic("no usable memory for early page pool");
    uintptr_t root=(uintptr_t)pml4;
    __asm__ volatile("mov %0,%%cr3"::"r"(root):"memory");
}
uintptr_t page_allocate(void) {
    uint64_t f=irq_save();
    for(size_t i=0;i<COUNT;i++) if(state[i]==1) {
        state[i]=2;free_pages--;
        uintptr_t p=i*PAGE;
        memset((void*)p,0,PAGE);
        irq_restore(f);return p;
    }
    irq_restore(f);return 0;
}
void page_release(uintptr_t p) {
    uint64_t f=irq_save();
    if(p%PAGE || p>=BOOT_IDENTITY_LIMIT || state[p/PAGE]!=2) panic("invalid page release");
    /* No dereference on release; data is erased on the next allocation.
     * Callers must relinquish every reference before releasing the page. */
    state[p/PAGE]=1;free_pages++;
    irq_restore(f);
}
uint64_t memory_free_pages(void) {return free_pages;}
