#ifndef SCOS_BOOT_H
#define SCOS_BOOT_H
#include <stdint.h>
#include <stddef.h>
#define BOOT_MAGIC UINT64_C(0x3436544f4f424353)
#define BOOT_VERSION 3
#define BOOT_MAP_MAX 512
/* The one contiguous block the stub hands the kernel, sized to what the layout below actually holds:
 * 256 KiB of prefix (handoff page, the EFI memory map copy, the module index at +200 KiB), a 1.5 MiB
 * page-table pool, and the 256 KiB module region at the top.  It used to be 16 MiB, which was not a
 * reserve for anything: the whole span is excluded from the page allocator (memory_init maps it
 * arena_start..table_end and never returns it), so an idle 128 MiB machine reported ~17 MiB of "kernel"
 * memory while 14 MiB of it held nothing at all, and new_table() could not be reached anyway because the
 * pool it needs is dozens of pages.  Table growth past the pool now falls back to the page allocator, so
 * the size is a startup convenience rather than a ceiling: see memory.c. */
#define BOOT_ARENA_SIZE (2ULL * 1024 * 1024)
#define GPU_MODULE_REGION (256ULL * 1024)
#define GPU_MODULE_AREA(arena) ((arena) + BOOT_ARENA_SIZE - GPU_MODULE_REGION)
#define BOOT_INDEX_ADDRESS(arena) ((arena) + 200ULL * 1024)
#define BOOT_INDEX_MAX 4096
#define KERNEL_SPAN_LIMIT UINT64_C(0x4000000)
#define PHYSICAL_LIMIT (UINT64_C(1)<<46)
/* Native UEFI descriptor prefix. The firmware's stride can exceed this size. */
/* UEFI wire order is type/pad, physical, virtual, pages, attributes. */
struct efi_memory {uint32_t type,pad;uint64_t physical,virtual_address,pages,attributes;};
struct boot_framebuffer {
    uint64_t base,size;
    uint32_t width,height,stride,red,green,blue;
};
struct boot_handoff {
    uint64_t magic;
    uint32_t version,size;
    uint64_t map_address,map_size,map_stride;
    uint32_t map_version,reserved;
    uint64_t kernel_start,kernel_end,arena_start,arena_size,rsdp;
    struct boot_framebuffer framebuffer;
    uint64_t tsc_hz;
    /* One GPU display module, selected by device id before the kernel exists.  The stub scans PCI
     * configuration space read-only, names the family from the same matcher the kernel uses, and
     * copies that one file into the executable region; every other module on the disk stays on the
     * disk.  module_state 0 = nothing selected, 1 = module bytes in place, 2 = the disk had no
     * module for the detected family, 3 = the read failed.  index_* describe the small directory
     * listing so the kernel can report how many modules exist but were never opened. */
    uint64_t module_address,module_bytes,index_address;
    uint32_t module_state,module_store_count,module_crc,module_index_size;
    char module_name[16];
};
_Static_assert(sizeof(struct efi_memory)==40,"UEFI memory descriptor prefix");
_Static_assert(sizeof(struct boot_handoff)==192,"handoff ABI v3");
_Static_assert(offsetof(struct boot_handoff,framebuffer)==88,"framebuffer ABI");
int boot_memory_usable(const struct efi_memory *);
int boot_map_valid(const struct efi_memory *,size_t);
/* Range queries consume a map already accepted by boot_map_valid. */
int boot_range_usable(const struct efi_memory *,size_t,uint64_t,uint64_t);
#endif
