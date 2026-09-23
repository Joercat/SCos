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
    char describe[256];
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
