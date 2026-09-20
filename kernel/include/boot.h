#ifndef SCOS_BOOT_H
#define SCOS_BOOT_H
#include <stdint.h>
#include <stddef.h>
#define BOOT_MAGIC UINT64_C(0x3436544f4f424353) /* SCBOOT64 */
#define BOOT_VERSION 1
#define BOOT_MAP_MAX 64
#define BOOT_IDENTITY_LIMIT UINT64_C(0x1000000)
struct boot_map_entry { uint64_t base, length; uint32_t type, attributes; };
struct boot_handoff {
    uint64_t magic;
    uint32_t version, size;
    uint64_t map_address;
    uint32_t map_count, map_stride;
    uint64_t kernel_start, kernel_end, kernel_file_end, bootstrap_cr3;
    uint32_t bios_drive, flags;
    uint64_t reserved;
};
_Static_assert(sizeof(struct boot_map_entry)==24, "E820 ABI");
_Static_assert(sizeof(struct boot_handoff)==80, "handoff ABI");
_Static_assert(offsetof(struct boot_handoff, kernel_start)==32, "handoff offset");
_Static_assert(offsetof(struct boot_handoff, bios_drive)==64, "handoff offset");
int boot_map_valid(const struct boot_map_entry *, size_t);
int boot_range_usable(const struct boot_map_entry *, size_t, uint64_t, uint64_t);
#endif
