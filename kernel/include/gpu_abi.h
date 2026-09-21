#ifndef SCOS_KERNEL_GPU_ABI_H
#define SCOS_KERNEL_GPU_ABI_H

/* The stable contract between SCos and a GPU display module.
 *
 * A GPU module is a small relocatable kernel-mode program, stored as its own file on the ESP
 * (\SCOS\DRIVERS\<FAMILY>.MOD), and loaded only when detection names that family on its own PCI
 * function.  This header is shared by three audiences and must stay free of kernel-internal
 * dependencies: the kernel loader (kernel/drivers/gpu/gpu_module.c), the module sources
 * (drivers/gpu/<family>/), and the packer (tools/build_gpu_module.py), which turns a linked
 * module object into the on-disk file this header describes.
 *
 * What a module may and may not do is deliberately narrow.  The module declares no undefined
 * symbols at all, so the loader never has to resolve an import: everything it can reach in the
 * kernel is one of the entries of struct gpu_module_exports, which is passed to its entry point.
 * That is a real structural guarantee, not a policy: modules provide their own memcpy/memset,
 * the packer rejects a module with any unresolved symbol, and the kernel's own exports are not
 * linked into the module image.  The consequence is that a module cannot be tricked or coerced
 * into calling an arbitrary kernel routine, and the loader's work is limited to relocation.
 *
 * What it is NOT: a sandbox.  Module code runs at ring 0 in the kernel's address space and, once
 * bound, may program the device's registers.  The validation below exists to catch a corrupted,
 * truncated, mismatched or ABI-skewed module before it can execute; it is not a defence against a
 * hostile module that someone has deliberately built and written to the boot disk.
 */

#include <stdint.h>
/* Spelled through stdint exactly like the kernel's own headers, so this file can be included on
 * either side of the boundary without a conflicting-typedef error. */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define SCOS_GPU_ABI_VERSION 1u

/* "SGPM" in byte order: a file whose first four bytes are not these is not a module. */
#define SCOS_GPU_MODULE_MAGIC 0x4d504753u
#define SCOS_GPU_MODULE_HEADER_SIZE 192u
#define SCOS_GPU_FAMILY_NAME 16

/* PCI-id rule, identical to the kernel's detection table.  A module carries its own copy so the
 * kernel can cross-check what the module claims against what the generated table knows. */
#define SCOS_GPU_ID_GENERIC 0x0001u
#define SCOS_GPU_ID_SUBVENDOR_MATCH 0x0002u

struct scos_gpu_pci_id {
    u32 vendor;
    u32 device;
    u32 flags;
    u32 subvendor;
    u32 subdevice;
};

/* Relocations.  There are exactly four kinds because a module has no imports: every reference is
 * to a location inside its own image, so each one is "place + addend, relative to the load
 * address".  Any other type is a build error in the packer, not something the loader has to
 * handle.  The numbers are the ELF x86-64 ones they mirror. */
enum scos_gpu_reloc_type {
    SCOS_GPU_RELOC_ABS64 = 1,
    SCOS_GPU_RELOC_ABS32S = 10,
    SCOS_GPU_RELOC_PC32 = 24,
    SCOS_GPU_RELOC_PLT32 = 4
};

struct scos_gpu_reloc {
    u32 offset;      /* byte offset inside the image where the value is written */
    u32 type;        /* scos_gpu_reloc_type */
    u32 symbol;      /* byte offset inside the image of the referred-to location */
    s32 addend;
};

/* On-disk module header, at file offset 0.  All sizes are from the start of the file; the three
 * blocks are laid out in this order: read/execute (text and rodata together, because a blitter
 * has no reason for a separate permission split), writable data, then a zero-filled .bss that
 * occupies no bytes in the file. */
struct scos_gpu_module_header {
    u32 magic;
    u32 abi;                    /* SCOS_GPU_ABI_VERSION; a mismatch is fatal and loud */
    u32 header_size;            /* SCOS_GPU_MODULE_HEADER_SIZE */
    u32 header_words;           /* checksum of this header's fields, see the packer */
    char family[SCOS_GPU_FAMILY_NAME];  /* NUL-padded, must equal the detection family name */
    u32 rxe_off, rxe_size;
    u32 data_off, data_size;
    u32 bss_size;
    u32 reloc_off, reloc_count;
    u32 id_off, id_count;       /* struct scos_gpu_pci_id[] inside the read/execute block */
    u32 entry_init;             /* int (*)(struct scos_gpu_exports *, struct scos_gpu_module **out) */
    u32 entry_teardown;         /* void (*)(struct scos_gpu_module *) */
    u32 image_align;            /* power of two, at least 8 */
    u32 load_size;              /* rxe + data: the bytes copied from the file */
    u32 image_size;             /* load_size + bss_size: the bytes the region must cover */
    u32 payload_crc;            /* CRC32 of everything from header_size to end of file */
    u32 module_min_bytes;       /* what the module itself thinks it needs, for the report */
    u32 flags;                  /* scos_gpu_module_flag */
    u32 reserved[23];
};

enum scos_gpu_module_flag {
    /* Reserved for a later tier: a module that may reprogram the display (mode set, plane,
     * palette, clocks).  No module in this tree sets it, and the kernel refuses the bit outright
     * rather than ignoring it, so the meaning cannot drift. */
    SCOS_GPU_MODULE_TAKES_DISPLAY = 1u << 0
};

/* What the kernel tells the module about the device it was loaded for.  The module is never given
 * a way to enumerate the bus itself: it gets exactly one function, named by the kernel's own
 * detection, and the module's ID list must cover that function. */
struct scos_gpu_device_info {
    u32 size;
    u16 vendor, device;
    u16 subvendor, subdevice;
    u8 bus, slot, function, revision;
    u32 class_hi, class_lo;
    u64 bar_base[6];
    u64 bar_bytes[6];
    u32 bar_is_io[6];
    u64 framebuffer_base;       /* the scanout surface the firmware validated */
    u64 framebuffer_bytes;
    u32 width, height, pitch;   /* pixels, and bytes per row */
    u32 bytes_per_pixel;
    u64 framebuffer_pointer;    /* the kernel's existing mapping of the scanout surface */
    u32 vram_bytes;             /* aperture size as the device reports it, 0 if unknown */
    u32 reserved;
};

/* The kernel's side of the contract.  Pass-through helpers only: each one exists so the module
 * does not need to touch page tables, port 0xcfc, or an unbounded wait loop of its own. */
struct scos_gpu_exports {
    u32 size;
    u32 abi;
    const struct scos_gpu_device_info *device;
    /* One line to the kernel log.  No format string crosses the boundary on purpose: the module
     * formats into its own buffer, so a module bug cannot crash a printf. */
    void (*log)(const char *line);
    /* Map device memory UC (write_combine 0) or WC (1).  Only the device the module was loaded for
     * may be mapped, and `offset' inside the handle is bounds-checked by the accessors below. */
    u64 (*map)(u64 physical, u64 bytes, u32 write_combine, u64 *mapped_bytes);
    u32 (*read32)(u64 handle, u32 offset);
    void (*write32)(u64 handle, u32 offset, u32 value);
    u64 (*ticks)(void);
    void (*spin)(u32 iterations);
    /* Bounded wait: returns 1 when the condition holds, 0 when the limit ran out.  The limit is
     * enforced by the kernel, so a module cannot hang the machine by spinning here. */
    u32 (*wait_bit)(u64 handle, u32 offset, u32 mask, u32 expect, u64 timeout_ticks);
    u64 (*alloc)(u64 bytes);     /* zeroed memory owned by the module, 0 on failure */
};

/* A drawing surface, in the device's own memory space.  The kernel decides where surfaces live
 * (which is the part that can damage a console) and the module only ever programs registers from
 * these numbers, so no offset chosen by a module can end up overlapping the visible scanout. */
struct scos_gpu_surface {
    u32 offset_bytes;           /* offset of the surface start inside the device aperture */
    u32 pitch_bytes;            /* bytes per row */
    u32 width, height;          /* pixels */
    u32 bytes_per_pixel;
    u32 reserved[3];
};

enum scos_gpu_surface_id {
    SCOS_GPU_SURFACE_FRONT = 0,  /* what the display controller is scanning out */
    SCOS_GPU_SURFACE_BACK = 1    /* the kernel's back buffer, in the same aperture */
};

/* What the module hands back.  Every function returns 0 on success and a negative value on
 * failure; on failure the kernel detaches the engine and resumes the CPU path, because a blitter
 * that fails half-way through a screen is worse than no blitter. */
struct scos_gpu_engine_ops {
    u32 size;
    u32 abi;
    void *context;
    /* Fill and copy are expressed in surface space so the module never needs to know the kernel's
     * layout: `surface' selects front or back, coordinates are pixels within that surface. */
    s32 (*fill)(void *context, u32 surface, s32 x, s32 y, s32 width, s32 height, u32 color);
    s32 (*copy)(void *context, u32 dst_surface, s32 dst_x, s32 dst_y, u32 src_surface,
                s32 src_x, s32 src_y, s32 width, s32 height);
    s32 (*wait_idle)(void *context);
    /* Where the kernel may place a back buffer, in aperture bytes.  Returning non-zero means
     * "no extra surface", which keeps the engine usable for fills only. */
    s32 (*vram_window)(void *context, u32 *offset_out, u32 *bytes_out);
    s32 (*set_surfaces)(void *context, const struct scos_gpu_surface *front,
                        const struct scos_gpu_surface *back);
    const char *(*describe)(void *context);
    void (*teardown)(void *context);
    /* Optional tier: a module may offer these by setting `size' accordingly.  The kernel only
     * calls an entry that falls inside the size the module reported. */
    s32 (*fill_span)(void *context, s32 y, const s32 *runs, u32 run_count, u32 color);
    s32 (*invert)(void *context, u32 surface, s32 x, s32 y, s32 width, s32 height);
};

/* The module's entry point receives the export table and returns its ops.  Returning non-zero
 * refuses the device (for example a chip the module recognises but does not drive) and leaves the
 * kernel on the CPU path with the refusal in the log. */
typedef int (*scos_gpu_module_init)(const struct scos_gpu_exports *exports,
                                    struct scos_gpu_engine_ops **out_ops);
typedef void (*scos_gpu_module_teardown)(struct scos_gpu_engine_ops *ops);

#endif
