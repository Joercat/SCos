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

struct gpu_pci_id {
    uint16_t vendor;
    uint16_t device;
    const char *name;      /* chip name from the upstream row, NULL if it had none */
};

/* What the upstream family's own hook table hands out.  This is an inventory of
 * the source we would port from, not a promise about SCos: `gpu_engine_*()` below
 * only succeeds once a family's code is actually linked and bound.  */
struct gpu_upstream_caps {
    unsigned engine2d : 1;    /* engine_token advertises B_2D_ACCELERATION */
    unsigned vsync : 1;       /* retrace semaphore hook returned directly */
    unsigned pan : 1;         /* MOVE_DISPLAY (CRTC start address) */
    unsigned cursor : 1;      /* SET_CURSOR_SHAPE */
    unsigned overlay : 1;     /* ALLOCATE_OVERLAY_BUFFER */
    unsigned fill : 1;        /* FILL_RECTANGLE */
    unsigned blit : 1;        /* SCREEN_TO_SCREEN_BLIT */
    unsigned span : 1;        /* FILL_SPAN */
    unsigned modeset : 1;     /* SET_DISPLAY_MODE */
    unsigned dpms : 1;        /* SET_DPMS_MODE */
};

struct gpu_match {
    const char *family;
    uint16_t primary_vendor;       /* first vendor in the table; 0 = no ID table.
                                    * Matching uses each row's own vendor instead, so a
                                    * family that pairs several rebranded vendors (nvidia)
                                    * still binds all of them. */
    uint8_t class_base;            /* 0xff = upstream does not test the class */
    uint8_t class_sub_a;
    uint8_t class_sub_b;           /* 0xff = unused */
    const struct gpu_pci_id *ids;
    uint16_t id_count;
    struct gpu_upstream_caps upstream;
    const char *features;          /* per-entry-point status, as measured */
    const char *source;            /* upstream file the table came from */
    const char *note;
};

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
    int is_scanout;                  /* this function feeds the visible console */
    uint64_t bar[6];                 /* raw BAR register values, never sized */
    int command, header_type;        /* read-only copies for the report */
};

#define GPU_MAX_DEVICES 8

void gpu_init(const struct boot_framebuffer *fb);
int gpu_device_count(void);
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
const struct gpu_match *gpu_match_family(int index);
int gpu_match_id_total(void);
/* Port state for a family name, or NULL when SCos has no record for it. */
const struct gpu_driver *gpu_port_for(const char *family);
int gpu_port_record_count(void);

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
