#ifndef SCOS_BOOT_H
#define SCOS_BOOT_H
#include <stdint.h>
#include <stddef.h>
#define BOOT_MAGIC UINT64_C(0x3436544f4f424353)
#define BOOT_VERSION 5   /* v5 adds the firmware record below; v4 had none */
/* The coprocessor's firmware set, as the boot stub found it.  Five roles because those are the files the
 * open-source driver for this chip loads (see boot/uefi/gsp_fw.h for the names and where they come from), and
 * a record rather than the bytes because the stub's only job here is to say what the boot volume holds:
 * reading 4 MiB of firmware into the boot arena would enlarge a reserve the kernel has to keep out of its
 * own allocator, for an image nothing consumes yet.  The states and flags are shared with the stub's
 * examiner so the kernel cannot disagree about what a number meant. */
#define BOOT_FIRMWARE_ROLES 5u
#define BOOT_FIRMWARE_STATE_ABSENT      0u  /* nothing by that name on the boot volume */
#define BOOT_FIRMWARE_STATE_STAGED      1u  /* read, sized, examined */
#define BOOT_FIRMWARE_STATE_TOO_BIG     2u  /* reserved: the stub's read caps at 16 MiB before anything can
                                             * be sized, so an oversized file is reported as unreadable and
                                             * this state stays unused rather than claiming a size */
#define BOOT_FIRMWARE_STATE_UNREADABLE  3u  /* the firmware's read failed: present too big, or an I/O
                                             * failure - not the same as absent, and not knowable further here */
#define BOOT_FIRMWARE_ELF64        1u       /* magic, class, byte order and entry size the loader's walk needs */
#define BOOT_FIRMWARE_PAYLOAD      2u       /* a `.fwimage' section: the payload the falcon is started from */
#define BOOT_FIRMWARE_SIGNATURE    4u       /* and the signature section this chip's driver names */
#define BOOT_FIRMWARE_BAD_MAGIC    8u       /* not an ELF at all */
#define BOOT_FIRMWARE_TRUNCATED   16u       /* a section table the file does not hold */
#define BOOT_FIRMWARE_NO_STRINGS  32u       /* the name table is outside the file: sections unreadable */
/* The five roles, in the order a report names them, and the 8.3 file on the boot volume each one is read
 * from.  A macro list rather than a table in some header, because the boot stub and the kernel have to say
 * the same names and the kernel cannot include a file from boot/uefi.  tools/makedisk.py writes these files
 * and tools/tests/test_gsp_firmware.py fails if any of the three ever drifts apart. */
#define BOOT_FIRMWARE_ROLE_NAMES { "gsp", "fmc", "bootloader", "booter load", "booter unload" }
#define BOOT_FIRMWARE_ROLE_FILES { "GSP.BIN", "GSPFMC.BIN", "GSPBL.BIN", "GSPBTL.BIN", "GSPBTU.BIN" }
struct boot_firmware_role {
    uint32_t state, bytes, crc32, flags;
    uint32_t payload_bytes, signature_bytes, sections, reserved;
};
struct boot_firmware {
    uint32_t staged, total_bytes, complete, flags;   /* flags: what went wrong, OR-ed across roles */
    char signature_name[24];        /* what the *gsp* image calls its signature section; empty when that
                                     * image carries none, which is the case the boot sequence would stop at */
    struct boot_firmware_role role[BOOT_FIRMWARE_ROLES];
};
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
    /* The stub's PCI walk, reported through rather than re-derived: how many functions it read, how many
     * buses it reached and how many of them were display functions.  A report that only says "no module"
     * cannot distinguish an empty bus from an unreached one, and those want different fixes. */
    uint32_t pci_functions_seen,pci_buses_scanned,pci_display_functions;
    char module_name[16];
    /* The firmware record.  Appended, so every field above keeps the offset the kernel was written against;
     * the size assertions below are what make "appended" safe rather than a hope. */
    struct boot_firmware firmware;
};
_Static_assert(sizeof(struct efi_memory)==40,"UEFI memory descriptor prefix");
/* 408: the v4 handoff was 208 bytes and the firmware record appended below it is 200, so no field a v4
 * kernel read at offset N reads it at any other offset - the size is asserted because the kernel refuses a
 * handoff of any other size, and a silent drift is a boot that panics for no reason a user could guess.
 * (The 208 it grew from was itself an assertion after three u32 fields moved the tail padding; the number
 * has caught every one of these mistakes so far.) */
_Static_assert(sizeof(struct boot_handoff)==408,"handoff ABI v5");
_Static_assert(sizeof(struct boot_firmware_role)==32,"firmware record entry ABI");
_Static_assert(sizeof(struct boot_firmware)==200,"firmware record ABI");
/* 204, not 208: module_name ended the v4 layout at 204 and the four bytes that padded it up to 208 were
 * nothing's to lose, so the record starts there and the size still lands on 408.  Both numbers are pinned,
 * because "no v4 field moved" is the entire claim that a v4 kernel and a v5 stub cannot disagree about what
 * offset the framebuffer lives at - and an offset that only works by luck is how a machine boots to a panic
 * nobody reading this header could explain. */
_Static_assert(offsetof(struct boot_handoff,firmware)==204,"firmware record follows v4 fields unchanged");
_Static_assert(offsetof(struct boot_handoff,framebuffer)==88,"framebuffer ABI");
int boot_memory_usable(const struct efi_memory *);
int boot_map_valid(const struct efi_memory *,size_t);
/* Range queries consume a map already accepted by boot_map_valid. */
int boot_range_usable(const struct efi_memory *,size_t,uint64_t,uint64_t);
#endif
