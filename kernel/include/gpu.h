/* GPU driver registry, auto-detection and the 2D engine boundary.
 *
 * Two rules govern this whole subsystem, and both come from what the target
 * machine needs to survive:
 *
 * 1. Matching is exact.  A family binds only to a vendor/device pair that the
 *    upstream driver for that family also binds to; the tables in
 *    `kernel/drivers/gpu/gpu_ids.h` are generated from those drivers' own
 *    `supportedDevices`/`kSupportedDevices`/`chipTable` arrays at a pinned
 *    commit, by `tools/research/gen_gpu_tables.py`.  Nothing here accepts a
 *    vendor as a proxy for a chip, and an unmatched ID is reported as unmatched
 *    rather than pushed at the closest driver.
 * 2. Detection never touches the display.  Identification reads configuration
 *    space only: no BAR sizing (which writes 0xffffffff into the BAR before
 *    restoring it), no reset, no power state, no clock or mode register.  Those
 *    operations can destroy the only usable console, so they live behind
 *    `gpu_driver_prepare()`, which nothing calls until a family's engine code
 *    has actually been ported and verified.  See `docs/migration/`
 *    `GPU-BASIC-2D-RESEARCH.md` for what has been measured about each family.
 */
#ifndef SCOS_GPU_H
#define SCOS_GPU_H

#include "kernel.h"
#include "gpu_match.h"
#include "gpu_abi.h"

/* Engine entry points, one per operation the compositor actually issues.  The
 * signatures mirror the accelerant engine ABI (fill rectangle, screen blit,
 * span fill, engine idle, token sync, retrace wait, CRTC pan) so a ported driver
 * can be dropped in without re-deriving its meaning; every one is optional.
 *
 * Contract for an implementation: coordinates are pixels in scanout space, the
 * colour is X8R8G8B8, and a call returns 0 only after the operation has been
 * submitted in a way the caller may immediately follow with `wait_idle`.  A
 * negative return means "this driver will not do this operation on this device",
 * and the caller must then do it on the CPU - never partially, never silently. */
struct gpu_engine_ops {
    int (*fill_rectangle)(int x, int y, int width, int height, uint32_t color);
    int (*screen_to_screen_blit)(int dst_x, int dst_y, int width, int height,
                                 int src_x, int src_y);
    int (*fill_span)(const int *runs, int run_count, uint32_t color);
    int (*wait_idle)(void);
    int (*retrace_wait)(void);
    int (*move_display)(int x, int y);
};

/* Port state of a family inside SCos, kept separate from the upstream inventory
 * so a log line can never imply more than the tree can do. */
enum gpu_port_state {
    GPU_PORT_NONE = 0,   /* matched family, no code ported yet -> CPU path */
    GPU_PORT_DISPLAY,    /* scanout/timing ported, no 2D engine */
    GPU_PORT_ENGINE      /* 2D engine ported; ops non-NULL */
};

struct gpu_device {
    uint8_t bus, slot, function, subclass;
    uint16_t vendor, device;
    uint8_t revision;
    const struct gpu_match *match;   /* family whose table matched, else NULL */
    const char *chip;                /* name from the matching row, may be NULL */
    int bound;                       /* a ported engine is attached to this device */
    const struct scos_gpu_engine_ops *module_ops;   /* loaded module's engine table, else NULL */
    int is_scanout;                  /* this function feeds the visible console */
    uint64_t bar[6];                 /* raw BAR register values, never sized */
    int command, header_type;        /* read-only copies for the report */
    uint8_t named_only;              /* row came from the naming table: a known chip, no driver claim */
    /* Real device access for a chip this tree has no engine for: the first two dwords of BAR0, read
     * through a mapping the kernel owns and never releases.  Probing is skipped for any function a
     * module could claim, because a second mapping of the same aperture would collide with the
     * module's own window.  reg_state: 0 not probed, 1 readable, 2 the device answered all-ones,
     * 3 the mapping was refused, 4 the function has memory decoding disabled in its command register. */
    uint64_t reg_base;
    uint32_t reg_first, reg_second;
    uint8_t reg_state;
    uint8_t reg_kernel_fallback;     /* the kernel read them only because no module was staged */
};

#define GPU_MAX_DEVICES 8

void gpu_init(const struct boot_framebuffer *fb);
int gpu_device_count(void);
/* Whether a device's match comes from a driver's binding table (may load a module) or from the
 * naming table (must not). */
int gpu_module_eligible(const struct gpu_device *g);
const struct gpu_device *gpu_device(int index);
const struct gpu_device *gpu_scanout_device(void);
/* The bound driver, i.e. the one family whose ported code SCos will use.  NULL
 * while no family is both matched and ported, which is the normal state. */
const struct gpu_driver *gpu_bound_driver(void);
void gpu_report(char *out, size_t capacity);
const char *gpu_vendor_name(uint16_t vendor);
/* PCI configuration writes issued while detection ran.  This must be 0: naming a
 * display adapter reads config space only.  It is reported rather than merely asserted
 * so a boot log shows the invariant held on the machine being debugged. */
int gpu_config_writes_during_detect(void);
/* The generated family table (kernel/drivers/gpu/gpu_ids.h), one record per family
 * SCos can name.  `gpu_match_id_total()` is the number of exact IDs across them. */
int gpu_match_family_count(void);
/* Port state for a family name, or NULL when SCos has no record for it. */
const struct gpu_driver *gpu_port_for(const char *family);
int gpu_port_record_count(void);

/* A GPU display module is a separate file on the boot disk; the firmware reads only the one whose
 * family the shared matcher names for this machine's chip.  The state below is what this boot
 * actually loaded and verified, so a report can distinguish "matched a family" from "a driver is
 * resident and proved to move pixels". */
struct gpu_module_state {
    int present;                     /* header validated, relocations applied */
    int bound;                       /* init succeeded and the kernel accepted the module */
    int identification_only;         /* ... and it exposes no engine, only what the chip said */
    char name[24];
    char family[16];
    /* Enough for a driver's identity line *and* the engine inventory it read from the chip.  This used to
     * be 512 and the sentence was written to fit it, which is how a real card's report lost its last
     * clause; a panel now wraps, so length costs lines rather than facts, and the bound only has to be
     * generous enough that no driver's whole inventory can reach it.  The kernel copies without truncating,
     * so the number is a ceiling to check against, not a place where text dies quietly.  It went to 1152
     * when the report grew its per-target clauses: each named block a chip refuses to answer costs about a
     * hundred and thirty bytes to say so, and how many blocks that is is the chip's decision, not the
     * driver's - so the fixture seeds its widest case and asserts the length, rather than this note. */
    char describe[1152];
    uint64_t resident_bytes;         /* image in the executable boot-arena region */
    uint64_t file_bytes;             /* bytes read off the disk */
    uint32_t store_count;            /* modules on the disk, including those never opened */
    uint32_t store_bytes;
    uint32_t opened_bytes;           /* the one file the firmware was told to read */
    uint32_t unopened_bytes;         /* every other family: on the disk, never loaded, never run */
    uint32_t reloc_count, id_count;
    int refusal;                     /* nonzero predicate code when present == 0 */
    int self_test_pixels, self_test_matches;
};
/* The coprocessor's firmware set as the boot volume offered it, passed up from the firmware through
 * struct boot_firmware.  Every number here is a measurement the kernel is repeating on the boot stub's
 * word, because the bytes themselves were read and released by the stub - boot/uefi/gsp_fw.h explains why
 * they are not kept - which is exactly why this struct carries what `gpu_firmware_state' recomputed rather
 * than what it copied: the counts are re-added from the per-file records here, and a record set whose own
 * summary disagrees with them is reported as inconsistent instead of quietly believed. */
#define GPU_FIRMWARE_ROLES BOOT_FIRMWARE_ROLES
struct gpu_firmware_role {
    unsigned state;                  /* BOOT_FIRMWARE_STATE_ABSENT / _STAGED / _TOO_BIG / _UNREADABLE */
    uint32_t bytes;                  /* as the stub read it off the disk */
    uint32_t crc32;                  /* over those bytes */
    uint32_t flags;                  /* BOOT_FIRMWARE_ELF64 and friends */
    uint32_t payload_bytes;          /* .fwimage, the payload the falcon is started from */
    uint32_t signature_bytes;        /* whatever the image calls its signature section */
    uint32_t sections;               /* section headers walked: 0 of them is a different fact from "missing one" */
    uint32_t offset;                 /* where its bytes were copied in the staged region, ~0u when kept=n */
};
/* What one staged image turned out to contain, read by this kernel out of the bytes the boot stub kept.
 * `resident' says the window exists; `checksum_matches' says it is still the file that was on the disk; the
 * offsets are inside the image, not inside the region, because the boot sequence is handed a physical address
 * per image and needs the section relative to it. */
struct gpu_firmware_image {
    unsigned resident;
    unsigned checksum_matches;
    uint32_t offset, bytes;
    unsigned elf32, elf64;
    unsigned fmc_template;             /* the first 52 bytes equal the published FMC header */
    unsigned fmc_sections;             /* type, flags, bounds and CRC32 of every section check out */
    unsigned fmc_named;                /* image, hash, publickey and signature are all present */
    unsigned sections;
    unsigned crc_failures;             /* sections whose sh_info CRC32 disagrees with their bytes */
    uint32_t section_begin;            /* where the FMC format says the first section's data starts */
    uint32_t payload_offset, payload_bytes;           /* .fwimage, from the RM image */
    uint32_t signature_offset, signature_bytes;       /* its .fwsignature_* section */
    uint32_t image_offset, image_bytes;               /* "image", from the FMC image */
    uint32_t hash_offset, hash_bytes;                   /* the three authentication sections of an FMC image */
    uint32_t publickey_offset, publickey_bytes;         /* are offsets too, because a boot sequence is handed */
    uint32_t fmc_signature_offset, fmc_signature_bytes; /* them physical addresses, not pointers */
    char signature_name[24];
};
struct gpu_firmware_state {
    int resident;                      /* images whose bytes this kernel can read */
    unsigned parsed;                   /* gpu_firmware_scan() ran over them */
    uint32_t resident_bytes;
    uint64_t address;                  /* where in physical memory, as the stub allocated it */
    struct gpu_firmware_image image[GPU_FIRMWARE_ROLES];
    int present;                     /* any role at all was found on the volume */
    unsigned staged;                 /* roles the stub could open, whatever the outcome */
    unsigned complete;               /* all five, and the first holds both sections the boot sequence reads */
    unsigned consistent;             /* the stub's summary agrees with the records it came with */
    uint32_t total_bytes, flags;
    char signature_name[24];         /* the name the staged image gives its signature section */
    struct gpu_firmware_role role[GPU_FIRMWARE_ROLES];
};
void gpu_firmware_note(const struct boot_handoff *handoff);
/* The same CRC32 the staged images are verified with, exposed because the handover payload the security
 * processor is given has to be checked over the copy the card will read, not over the copy this kernel read. */
uint32_t gpu_firmware_crc32(const uint8_t *data, uint32_t length);

#define GPU_FSP_PAYLOAD_BYTES 860              /* one NVDM_PAYLOAD_COT, exactly, padded by the sender
                                                  * to a whole number of dwords before it is written */
/* The kernel's only raw access to a device register: `window' is what device_map() returned, `within' a byte
 * offset inside it.  Two functions and no policy - bounds, counting and the decision to stop belong to the
 * caller, where they can be tested with the logic that needs them - so that a caller whose registers have
 * behaviour (this card's channel offset walks as it is read, and its queue pointers move when the firmware
 * consumes a packet) can be run against a model of the register file instead of against an array.
 * kernel/drivers/gpu/gpu_io.c is the production pair; tools/tests/gpu_fsp_host.c supplies its own. */
uint32_t gpu_io_read32(const volatile uint32_t *window, uint64_t within);
void gpu_io_write32(volatile uint32_t *window, uint64_t within, uint32_t value);

/* What the card's flight security processor said when this OS asked.  Every field is a register read, a byte
 * count or a staging decision; nothing here is inferred, and a zero `reads' means the file never got as far as
 * touching the hardware.  See kernel/drivers/gpu/gpu_fsp.c for what each number is and who published it. */
struct gpu_fsp_state {
    int present, attempted;                  /* a display function was there; the probe ran on it */
    int nvidia;                              /* the vendor is the one whose decode these offsets are */
    uint16_t vendor;                         /* whose it was, whichever it turned out to be */
    uint64_t bar0;                           /* the aperture the three pages below are offsets into */
    int skipped;                             /* 1 no aperture at all, 2 one that cannot be mapped */
    int mapped, mailbox_readable;            /* the windows exist; the queue registers answer at all */
    int gsp_readable;                        /* the coprocessor's own block no longer refuses a reader */
    uint32_t boot_scratch;                   /* 0xFF in the low byte once the FSP has finished booting */
    int booted, boot_polls;                   /* and how many looks at it the wait took */
    uint32_t ucode_version;                  /* NV_PFSP_FSP_UCODE_VERSION, 11:0 */
    uint32_t fuse_check;                     /* the FSP's own fuse-consistency word: 0 means it passed */
    uint32_t hwcfg2;                         /* the capability word whose refusal was the blocker */
    uint32_t queue_head, queue_tail, msgq_head, msgq_tail;
    int queried, send_blocked;               /* a request went into the channel; why one could not */
    int polled, reply_polls;                  /* how many looks each of the two waits took */
    uint32_t packet_bytes, doorbell;
    int responded, framed_ok, reply_som, reply_eom, reply_oversize, reply_delta_ok;
    uint32_t reply_bytes;
    int reply_type, reply_kind;              /* the NVDM type in the header, and the payload's first byte */
    uint32_t reply_task, reply_command, reply_error;
    int reply_complete;
    uint64_t reads, writes;                  /* register accesses this file made, counted as it went */
    uint32_t refusal_reads;                  /* how many of the reads were the 0xbadf4100 refusal */
    int fmc_seen, staged, cot_ready, cot_defect;
    uint64_t image_bytes, image_physical, image_crc32;
    uint8_t payload[GPU_FSP_PAYLOAD_BYTES];  /* the handover, assembled and never sent */
};
void gpu_fsp_note(const struct gpu_device *device, const struct boot_handoff *handoff);
const struct gpu_fsp_state *gpu_fsp_state(void);
void gpu_firmware_scan(struct gpu_firmware_state *f, const uint8_t *base, uint32_t total);
const struct gpu_firmware_state *gpu_firmware_state(void);
const char *gpu_firmware_role_name(unsigned role);
const struct gpu_module_state *gpu_module_state(void);
const struct scos_gpu_engine_ops *gpu_module_ops(void);
int gpu_module_bind(struct gpu_device *device, const struct boot_handoff *handoff);
int gpu_module_fill(int x, int y, int width, int height, uint32_t color);
int gpu_module_copy(int dst_x, int dst_y, int src_x, int src_y, int width, int height);
int gpu_module_wait_idle(void);
void gpu_module_report(char *out, size_t capacity, const struct boot_handoff *handoff);

/* Engine boundary used by the compositor.  Each returns 0 on success and a
 * negative value when no engine is bound or the bound driver declines the
 * operation, which is the caller's cue to run its own CPU path. */
int gpu_engine_fill_rectangle(int x, int y, int width, int height, uint32_t color);
int gpu_engine_screen_to_screen_blit(int dst_x, int dst_y, int width, int height,
                                     int src_x, int src_y);
int gpu_engine_wait_idle(void);
int gpu_engine_retrace_wait(void);
int gpu_engine_move_display(int x, int y);
/* 1 when an engine is bound and willing to try the operation; used to choose a
 * path, never to claim a result. */
int gpu_engine_available(void);
/* Whether the engine's front surface is the memory the console actually scans out.  A bound engine
 * on a *second* card drives its own aperture only, and painting the desktop there would move pixels
 * nobody can see; the compositor asks this before it hands a rectangle to the GPU. */
int gpu_engine_drives_output(void);
/* PCI Express link state of the i'th detected display function, read from its own capability
 * structure: 1 when generation and lanes hold what the slot negotiated, 0 when the function has no
 * Express capability at all (which is a real answer for a device behind a conventional slot). */
int gpu_link_state(int index, unsigned *generation, unsigned *lanes);

/* Every accelerated drawing path reports what the engine did and what the CPU kept for itself, in
 * source pixels.  The two counters exist so "the GPU is doing the drawing" is a measured statement in
 * the boot report and the tests, not an inference from the fact that a driver loaded. */
void gpu_engine_note_work(uint32_t by_engine, uint32_t by_cpu);
void gpu_engine_work_totals(uint32_t *by_engine, uint32_t *by_cpu);

struct gpu_driver {
    const char *family;
    enum gpu_port_state state;
    const struct gpu_engine_ops *ops;
    /* What still stands between this family and GPU work in SCos, stated from the
     * measured source rather than assumed.  Printed by `gpu_report()` so a log
     * explains a fallback instead of hiding it. */
    const char *gap;
    /* Called only when the family is matched *and* SCos is allowed to own the
     * display; NULL while no port exists.  May map MMIO, never modeset. */
    int (*prepare)(struct gpu_device *device);
};

#endif /* SCOS_GPU_H */
