/* Rage 128 / Mach64 register definitions for the SCos GPU module.
 *
 * Values transcribed from two sources that agree, and both are checked rather than recalled:
 *   Haiku 7be0fef07df0 src/add-ons/accelerants/ati/rage128.h (MIT; originally the X.org ATI
 *     driver, "Copyright 1999, 2000 ATI Technologies Inc., Precision Insight, VA Linux")
 *   QEMU master c1c18d1e640b hw/display/ati_regs.h, which is the model SCos verifies against.
 * Only the registers this engine touches are carried here; a module that needs another register
 * adds it with both citations.
 */
#ifndef SCOS_DRIVERS_GPU_ATI_REGS_H
#define SCOS_DRIVERS_GPU_ATI_REGS_H

#define R128_GEN_RESET_CNTL            0x00f0
#define   R128_SOFT_RESET_GUI            (1u << 0)
#define R128_CONFIG_MEMSIZE            0x00f8
#define R128_CRTC_GEN_CNTL             0x0030
#define R128_PC_NGUI_CTLSTAT           0x0184
#define   R128_PC_FLUSH_ALL              0x00ffu
#define   R128_PC_BUSY                   (1u << 31)
#define R128_SRC_Y_X                   0x1434
#define R128_DST_Y_X                   0x1438
#define R128_DST_HEIGHT_WIDTH          0x143c
#define R128_DP_GUI_MASTER_CNTL        0x146c
#define   R128_GMC_SRC_PITCH_OFFSET_CNTL (1u << 0)
#define   R128_GMC_DST_PITCH_OFFSET_CNTL (1u << 1)
#define   R128_GMC_BRUSH_NONE            (15u << 4)
#define   R128_GMC_BRUSH_SOLID_COLOR     (13u << 4)
#define   R128_GMC_SRC_DATATYPE_COLOR    (3u << 12)
#define   R128_GMC_DST_DATATYPE_SHIFT    8
#define   R128_GMC_CLR_CMP_CNTL_DIS      (1u << 28)
#define   R128_GMC_AUX_CLIP_DIS          (1u << 29)
#define   R128_GMC_DP_SRC_SOURCE_MEMORY  (2u << 24)
#define   R128_ROP3_Dn                   0x00550000u   /* dest = ~source: the invert path */
#define   R128_ROP3_P                    0x00f00000u   /* pattern copy = solid fill */
#define   R128_ROP3_S                    0x00cc0000u   /* source copy = bitblt     */
#define R128_DST_OFFSET                0x1404
#define R128_DST_PITCH                 0x1408
#define R128_SRC_OFFSET                0x15ac
#define R128_SRC_PITCH                 0x15b0
#define R128_DP_BRUSH_FRGD_CLR         0x147c
#define R128_DP_SRC_FRGD_CLR           0x15d8
#define R128_DP_SRC_BKGD_CLR           0x15dc
#define R128_DP_CNTL                   0x16c0
#define   R128_DST_X_LEFT_TO_RIGHT       (1u << 0)
#define   R128_DST_Y_TOP_TO_BOTTOM       (1u << 1)
#define R128_DP_DATATYPE               0x16c4
#define   R128_HOST_BIG_ENDIAN_EN        (1u << 29)
#define R128_DP_WRITE_MASK             0x16cc
#define R128_AUX_SC_CNTL               0x1660
#define R128_DEFAULT_OFFSET            0x16e0
#define R128_DEFAULT_PITCH             0x16e4
#define R128_DEFAULT_SC_BOTTOM_RIGHT   0x16e8
#define   R128_DEFAULT_SC_RIGHT_MAX      0x1fffu
#define   R128_DEFAULT_SC_BOTTOM_MAX     (0x1fffu << 16)
#define R128_SC_TOP_LEFT               0x16ec
#define R128_SC_BOTTOM_RIGHT           0x16f0
#define R128_GUI_STAT                  0x1740
#define   R128_GUI_FIFOCNT_MASK          0x0fffu
#define   R128_GUI_ACTIVE                (1u << 31)
#define R128_GUI_PROBE                 0x16bc
#define R128_SCALE_3D_CNTL             0x1a00
#define R128_DST_BRES_ERR              0x1628
#define R128_DST_BRES_INC              0x162c
#define R128_DST_BRES_DEC              0x1630

/* Upstream's own wait bound (Haiku rage128.h R128_TIMEOUT); kept as a named constant but used as
 * a *cap*, not as a retry loop that resets the engine forever.  See the deviations note at the top
 * of module.c. */
#define R128_FIFO_LIMIT                2000000u

#define R128_DST_WIDTH_HEIGHT              0x1598       /* Haiku rage128.h:116 */
#define R128_DP_BRUSH_BKGD_CLR             0x1478       /* Haiku rage128.h:91 / QEMU ati_regs.h:194 */

#endif
