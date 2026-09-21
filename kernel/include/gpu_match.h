/* Shared GPU identity matching.
 *
 * This header is compiled into the kernel *and* into the UEFI boot stub, so it must not depend on
 * kernel internals.  The sharing is deliberate: the stub has to name a GPU family in order to pick
 * which single module to read off the disk, and if it carried its own copy of the rules the two
 * views could drift apart.  That is the worst failure mode available here -- the kernel would reject
 * the module the firmware loaded, or accept one it should not have, and the machine would quietly
 * run on the wrong path.  Both sides therefore call the same matcher over the same generated tables
 * (kernel/drivers/gpu/gpu_ids.h, produced by tools/research/gen_gpu_tables.py).
 */
#ifndef SCOS_GPU_MATCH_H
#define SCOS_GPU_MATCH_H

#include <stdint.h>

struct gpu_pci_id {
    uint16_t vendor;
    uint16_t device;
    const char *name;      /* chip name from the upstream row, NULL if it had none */
};

/* What the upstream family's own hook table hands out.  This is an inventory of the source we
 * would port from, not a promise about SCos: `gpu_engine_*()` only succeeds once a family's code
 * is linked in or its module has been loaded, validated and bound. */
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

/* The family whose table binds this function, and the row that bound it.  NULL means no upstream
 * table knows this id, which is a report rather than an error: the device stays on the CPU path. */
const struct gpu_match *gpu_match_device(uint16_t vendor, uint16_t device, uint8_t subclass,
                                         const struct gpu_pci_id **row_out);
const char *gpu_family_name(const struct gpu_match *match);

/* The generated family table, one record per family SCos can name.  `gpu_match_id_total()` is the
 * number of exact IDs across them. */
int gpu_match_family_count(void);
const struct gpu_match *gpu_match_family(int index);
int gpu_match_id_total(void);

#endif
