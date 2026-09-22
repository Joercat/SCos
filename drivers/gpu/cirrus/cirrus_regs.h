/*
 * Cirrus Logic CL-GD5446 bitBLT register offsets, as the block appears in the function's
 * memory-mapped register BAR.
 *
 * These are the offsets the chip's own manual assigns to the MMIO page (the same registers exist at
 * I/O ports for legacy drivers, which this module does not need because the kernel hands out memory
 * mappings only).  The layout was read off the device model in QEMU 11.0 (hw/display/cirrus_vga.c:
 * CIRRUS_MMIO_BLTBGCOLOR 0x00 ... CIRRUS_MMIO_BLTSTATUS 0x40), which is also the implementation this
 * driver is verified against.  Nothing here is copied from an operating system's driver: the register
 * numbers, the bit definitions and the programming order are this file's own work.
 */
#ifndef SCOS_CIRRUS_REGS_H
#define SCOS_CIRRUS_REGS_H

/* The block's base inside the register BAR, measured against the device model rather than assumed:
 * the first 256 bytes of that BAR are the legacy VGA registers remapped into memory (index/data
 * pairs at 0x1e/0x1f and so on), and the bitBLT registers begin after them.  Writing them at offset
 * 0 instead is silently harmless -- it pokes the VGA port window and the engine never starts -- which
 * is exactly the kind of failure a read-back self-test exists to catch. */
#define CR_BLOCK              0x100

#define CR_BLT_BG_COLOR      (CR_BLOCK + 0x00)   /* dword, colour the transparent compare matches against */
#define CR_BLT_FG_COLOR        (CR_BLOCK + 0x04)   /* dword, solid colour for a fill, and the copy's ROP source */
#define CR_BLT_WIDTH           (CR_BLOCK + 0x08)   /* word, pixels in the row minus one */
#define CR_BLT_HEIGHT          (CR_BLOCK + 0x0a)   /* word, rows minus one */
#define CR_BLT_DEST_PITCH      (CR_BLOCK + 0x0c)   /* word, bytes per row at the destination */
#define CR_BLT_SRC_PITCH       (CR_BLOCK + 0x0e)   /* word, bytes per row at the source */
#define CR_BLT_DEST_ADDR       (CR_BLOCK + 0x10)   /* dword, byte offset in video memory */
#define CR_BLT_SRC_ADDR        (CR_BLOCK + 0x14)   /* dword, byte offset in video memory (or in system memory) */
#define CR_BLT_WRITE_MASK      (CR_BLOCK + 0x17)   /* byte, per-plane write enable */
#define CR_BLT_MODE            (CR_BLOCK + 0x18)   /* byte, see the bits below */
#define CR_BLT_ROP             (CR_BLOCK + 0x1a)   /* byte, raster operation */
#define CR_BLT_MODE_EXT        (CR_BLOCK + 0x1b)   /* byte */
#define CR_BLT_TRANSP_COLOR    (CR_BLOCK + 0x1c)   /* dword */
#define CR_BLT_TRANSP_MASK     (CR_BLOCK + 0x20)   /* dword */
#define CR_BLT_STATUS          (CR_BLOCK + 0x40)   /* byte: read bit 0 busy; write bit 1 start, bit 2 reset */

/* The unit a driver actually writes.  This block has no 32-bit registers: the entries above are
 * bytes and words at adjacent offsets, and a 32-bit store is delivered to the device as four
 * independent byte writes (QEMU's model declares max_access_size 1 and splits; the chip samples each
 * byte lane on its own).  So registers are programmed in aligned groups, one store per group, and a
 * group is never written twice in the same operation -- writing CR_BLT_WIDTH on its own as a dword
 * would land on the height bytes above it as well. */
#define CR_BLT_SIZE            (CR_BLOCK + 0x08)   /* 0x08 width-1, 0x0a height-1, both words */
#define CR_BLT_PITCH           (CR_BLOCK + 0x0c)   /* 0x0c destination pitch, 0x0e source pitch, bytes */
#define CR_BLT_SRC_AND_MASK    (CR_BLOCK + 0x14)   /* 0x14..0x16 source address, 0x17 write mask */
#define CR_BLT_MODE_SET        (CR_BLOCK + 0x18)   /* 0x18 mode, 0x19 reserved, 0x1a rop, 0x1b extended mode */
#define CR_BLT_DEST_TOP_BYTE   (CR_BLOCK + 0x13)   /* 0x10..0x12 are the address; this byte is reserved on the chip */

#define BLT_MODE_BACKWARDS     0x01
#define BLT_MODE_SYS_DEST      0x02
#define BLT_MODE_SYS_SRC       0x04
#define BLT_MODE_TRANSPARENT   0x08
#define BLT_MODE_PATTERN       0x40
#define BLT_MODE_COLOR_EXPAND  0x80
#define BLT_MODE_32BPP         0x30   /* pixel width field (31:4? no: bits 5:4) == 3 */
#define BLT_MODE_WIDTH_MASK    0x30

#define BLT_START              0x02
#define BLT_BUSY               0x01
#define BLT_RESET              0x04

#define BLT_EXT_SOLID_FILL     0x04

#define ROP_COPY               0x0d   /* dst = src, the only raster operation this driver uses */

#endif
