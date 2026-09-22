/* Per-family port state: what SCos can actually do once a family is detected.
 *
 * This table is the seam between auto-detection and driver code.  Detection can
 * name a chip today because the ID tables are generated from the upstream drivers;
 * a family only reaches the GPU path when it has an entry here whose `ops` point at
 * real, ported code.  Every entry below therefore states its own limit, in terms of
 * what has been measured in `docs/migration/GPU-BASIC-2D-RESEARCH.md`: the size of
 * the 2D core, whether the upstream family even advertises `B_2D_ACCELERATION`, and
 * which hooks upstream hands out conditionally, denies, or leaves commented out.
 *
 * Adding a port means filling in `ops`/`prepare` here and deleting its `gap` text,
 * not editing the generated ID tables.
 */
#include "scos.h"
#include "gpu.h"

#define GPU_PORT(fam, why) \
    { .family = fam, .state = GPU_PORT_NONE, .ops = 0, .prepare = 0, .gap = why }

static const struct gpu_driver ports[] = {
    GPU_PORT("radeon",
        "2D core measured at 1,173 lines (Acceleration.c + CP.c, R100-R480); needs "
        "an FB-resident command ring and CRTC scanout ownership before use"),
    GPU_PORT("nvidia",
        "engine measured at 3,949 lines (NV04-NV48); hardware cursor is gated on the "
        "upstream hardcursor setting; needs its own PLL/CRTC init"),
    GPU_PORT("matrox",
        "engine measured at 496 lines (G100-G550); Parhelia is not in the table; "
        "cursor gated on hardcursor"),
    GPU_PORT("via",
        "engine measured at 270 lines; one table row (0x3344) is commented out "
        "upstream, so that chip is not matched"),
    GPU_PORT("neomagic",
        "engine measured at 455 lines; cursor gated on hardcursor"),
    GPU_PORT("s3",
        "engine measured at 710 lines across Trio64/ViRGE/Savage; cursor direct"),
    GPU_PORT("ati",
        "engine measured at 496 lines (Mach64/Rage128); this is the family QEMU's "
        "ati-vga device presents (1002:5046), so it is the one case verifiable by "
        "emulation end to end"),
    GPU_PORT("3dfx",
        "engine measured at 153 lines; the retrace hook is denied upstream, so no "
        "vsync even after a port"),
    GPU_PORT("et6x00",
        "engine measured at 258 lines (Tseng ET6000/ET6300); retrace hook commented "
        "out upstream; modes capped at 1024x768"),
    GPU_PORT("intel_810",
        "engine measured at 83 lines and covers i810-i815 only; no retrace hook"),
    GPU_PORT("intel_extreme",
        "engine token advertises no B_2D_ACCELERATION and the BLT wait path returns "
        "early for the Lake families, so this buys the display tier only; scanout "
        "needs a write-combined aperture mapping, an IRQ source, and the device ID "
        "added for the target chip"),
    GPU_PORT("radeon_hd",
        "engine token advertises no B_2D_ACCELERATION and 119 of its 459 table rows "
        "sit inside #if 0; display/AtomBIOS code is the bulk of the 32,060-line "
        "closure"),
    GPU_PORT("vesa",
        "no 2D engine: firmware VBE gives modes and a linear framebuffer, not a "
        "blitter"),
    GPU_PORT("cirrus",
        "SCos-authored CL-GD5446 bitBLT module: solid fills and screen-to-screen copies in the "
        "aperture the console scans out; no 3D, no modeset, no system-memory BLT (the chip's FIFO "
        "costs more to feed than a write-combined store), no cursor"),
    GPU_PORT("framebuffer",
        "no 2D engine, and the retrace hook is a stub returning -1, so this is "
        "exactly the path SCos is on now"),
    GPU_PORT("virtio",
        "no 2D engine in the Haiku accelerant; host-side copies, and SCos has no "
        "virtio transport"),
};

const struct gpu_driver *gpu_port_for(const char *family)
{
    if (!family) return 0;
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        if (strcmp(ports[i].family, family) == 0) return &ports[i];
    return 0;
}

int gpu_port_record_count(void)
{
    return (int)(sizeof(ports) / sizeof(ports[0]));
}
