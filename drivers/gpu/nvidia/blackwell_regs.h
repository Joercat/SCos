/*
 * The registers SCos reads on an NVIDIA Blackwell display function - one dword, read once.
 *
 * Provenance.  The offset and all four field positions below are NVIDIA's own published definitions,
 * MIT licensed, taken from `manuals/ampere/ga100/dev_boot.ref.txt` in NVIDIA/open-gpu-doc:
 *
 *   NV_PMC_BOOT_0                0x00000000 (R--4R)              read only, 32 bit, aligned
 *   NV_PMC_BOOT_0_MINOR_REVISION 3:0
 *   NV_PMC_BOOT_0_MAJOR_REVISION 7:4
 *   NV_PMC_BOOT_0_IMPLEMENTATION 23:20
 *   NV_PMC_BOOT_0_ARCHITECTURE   28:24
 *
 * Nothing here was copied from a driver: neither NVIDIA's own kernel driver (nouveau, the open
 * GSP-RM kernel modules, or any Resource Manager source) nor a Linux, Windows or Haiku driver was
 * read while writing this file.  Where a register *name* is used, it is the name NVIDIA publishes,
 * because a name the silicon's own documentation uses is the only one worth repeating.
 *
 * Why the field table is the Ampere one: `manuals/` in the published documentation stops at Ampere -
 * for Blackwell, NVIDIA publishes only short addenda (src/common/inc/swref/published/blackwell/gb202/
 * in the open kernel-modules tree), not a full register database.  The fields above are stable across
 * those generations, and the driver's behaviour does not depend on that assumption: it prints what it
 * reads and never *requires* a particular value.  A chip that answers with an architecture the newest
 * published table does not name is reported as such instead of being refused, because the number that
 * matters to a user is which chip is in the machine, and the number that matters to a driver author is
 * whether the register block is alive at all.  A block that returns all zeroes or all ones is refused:
 * that is a function whose memory decoding the boot has not enabled, or a device in a state the
 * firmware left behind, and no inventory claim should be made from it.
 */
#ifndef SCOS_DRIVER_NVIDIA_BLACKWELL_REGS_H
#define SCOS_DRIVER_NVIDIA_BLACKWELL_REGS_H

/* Power management and clock block, first dword of the register aperture. */
#define NV_PMC_BOOT_0                 0x00000000u

#define NV_PMC_BOOT_0_MINOR_REVISION(v)   (((v) >> 0) & 0xfu)    /* 3:0,  INIT, _1 ... _11 */
#define NV_PMC_BOOT_0_MAJOR_REVISION(v)   (((v) >> 4) & 0xfu)    /* 7:4,  A ... F */
#define NV_PMC_BOOT_0_IMPLEMENTATION(v)   (((v) >> 20) & 0xfu)   /* 23:20 */
#define NV_PMC_BOOT_0_ARCHITECTURE(v)    (((v) >> 24) & 0x1fu)   /* 28:24 */

/* The architecture numbers the newest published manual names.  Anything else - including the number a
 * Blackwell chip answers - is printed as a number rather than translated, since inventing a mapping
 * here would be a guess dressed up as a fact. */
#define NV_PMC_BOOT_0_ARCHITECTURE_GV100  0x14u   /* Volta   */
#define NV_PMC_BOOT_0_ARCHITECTURE_TU100  0x16u   /* Turing  */
#define NV_PMC_BOOT_0_ARCHITECTURE_GA100  0x17u   /* Ampere  */

/* The user-mode window: the block a submission is rung through, and the copy of the GPU's own clock that
 * is readable without any privileged setup.  Published for Volta and Turing in
 * manuals/turing/tu104/dev_usermode.ref.txt (offsets inside the register BAR); NVIDIA publishes no
 * equivalent for Blackwell, which is why everything below is read and reported rather than assumed - the
 * class register says from the device whether this window exists on this chip and under what class number,
 * and the clock in the same page says whether the block is live.  Nothing here is ever written:
 * NV_USERMODE_NOTIFY_CHANNEL_PENDING is the doorbell, and ringing it with no channel set up is one way to
 * hang a card, so the address is named for the reader and left alone by the code. */
#define NV_USERMODE_CFG0                       0x00810000u   /* R--4R, USERMODE_CLASS_ID 15:0 */
#define NV_USERMODE_CFG0_CLASS_ID(v)                ((v) & 0xffffu)
#define NV_USERMODE_CLASS_ID_VOLTA_TURING           0xc461u   /* the documented reset value */
#define NV_USERMODE_TIME_0                     0x00810080u   /* R--4R, low 32 bits, 32 ns granularity */
#define NV_USERMODE_TIME_1                     0x00810084u   /* R--4R, upper 29 bits */
#define NV_USERMODE_NOTIFY_CHANNEL_PENDING     0x00810090u   /* -W-4R: named, never written here */
#define NV_USERMODE_WINDOW_BYTES               0x00020000u   /* the window's own size, 0x81FFFF:0x810000 */

/* The device-inventory array: the chip's own list of the engines it contains, which runlist each one
 * sits on, and where each engine's own register block begins.  NVIDIA publishes the array for Turing in
 * manuals/turing/tu104/dev_top.ref.txt:
 *
 *   NV_PTOP_DEVICE_INFO(i)   0x00022700+i*4   (R--4A, __SIZE_1 64 entries)
 *       CHAIN  31:31  set means the next entry describes the same device as this one
 *       ENTRY   1:0   0 NOT_VALID, 1 DATA, 2 ENUM, 3 ENGINE_TYPE - which decides how to read the rest
 *       DATA:  PRI_BASE 23:12 (shift by _ALIGN 12), INST_ID 29:26, FAULT_ID 9:2
 *       ENUM:  ENGINE_ENUM 29:26, RUNLIST_ENUM 24:21, INTR_ENUM 19:15, RESET_ENUM 13:9,
 *              gated by valid bits ENGINE 5, RUNLIST 4, INTR 3, RESET 2
 *       ENGINE_TYPE: TYPE_ENUM 30:2 -> GRAPHICS 0, COPY0 1, COPY1 2, COPY2 3, MSPDEC 8, MSPPP 9,
 *              MSVLD 10, MSENC 11, VIC 12, SEC 13, NVENC0 14, NVENC1 15, NVDEC 16, IOCTRL 18,
 *              LCE 19, GSP 20, NVJPG 21
 *
 * Ampere keeps the same format one page further along and adds a header
 * (src/common/inc/swref/published/ampere/ga100/dev_top.h):
 *
 *   NV_PTOP_DEVICE_INFO2(i)  0x00022800+i*4   (R--4A),  with ..._DEV_TYPE_ENUM_LCE 19
 *   NV_PTOP_DEVICE_INFO_CFG  0x000224FC: MAX_DEVICES 15:4, MAX_ROWS_PER_DEVICE 19:16, NUM_ROWS 31:20
 *
 * For Blackwell NVIDIA publishes neither address.  Its entire top addendum
 * (swref/published/blackwell/gb202/dev_top_zb.h) is one line,
 * NV_PTOP_ZB_DEVICE_INFO_DEV_TYPE_ENUM_LCE 0x13 - the same LCE number as Turing and Ampere - which says
 * the entry *format* carried forward while the *address* left the published set.  So both candidate
 * offsets are asked here and the one whose entries decode consistently is the one reported.  A table that
 * decodes is a measurement of this chip; silence means the block moved, not that the engines are absent,
 * and the two answers are worded differently below for that reason.  Everything in this block is read-only
 * in the documentation (R--4A / R--4R) and is only ever read: the numbers say which engine to build a
 * submission for, and nothing here can reprogram one.
 */
#define NV_PTOP_DEVICE_INFO_CFG              0x000224fcu
#define NV_PTOP_DEVICE_INFO_TURING           0x00022700u   /* NV_PTOP_DEVICE_INFO(0) */
#define NV_PTOP_DEVICE_INFO_AMPERE           0x00022800u   /* NV_PTOP_DEVICE_INFO2(0) */
#define NV_PTOP_DEVICE_INFO__SIZE_1          64u

#define NV_PTOP_CFG_MAX_DEVICES(v)                (((v) >> 4) & 0xffu)     /* 15:4  */
#define NV_PTOP_CFG_MAX_ROWS_PER_DEVICE(v)        (((v) >> 16) & 0xfu)     /* 19:16 */
#define NV_PTOP_CFG_NUM_ROWS(v)                   (((v) >> 20) & 0xfffu)   /* 31:20 */

#define NV_PTOP_CHAIN_BIT                    0x80000000u   /* 31:31, CHAIN_ENABLE */
#define NV_PTOP_ENTRY(v)                      ((v) & 3u)    /* 1:0 */
#define NV_PTOP_ENTRY_NOT_VALID                0u
#define NV_PTOP_ENTRY_DATA                     1u
#define NV_PTOP_ENTRY_ENUM                     2u
#define NV_PTOP_ENTRY_ENGINE_TYPE              3u

#define NV_PTOP_DATA_PRI_BASE(v)                    (((v) >> 12) & 0xfffu)  /* 23:12 */
#define NV_PTOP_DATA_PRI_BASE_ALIGN                 12u
#define NV_PTOP_DATA_INST_ID(v)                     (((v) >> 26) & 0xfu)    /* 29:26 */
#define NV_PTOP_ENUM_ENGINE(v)                      (((v) >> 26) & 0xfu)    /* 29:26 */
#define NV_PTOP_ENUM_RUNLIST(v)                     (((v) >> 21) & 0xfu)    /* 24:21 */
#define NV_PTOP_ENUM_ENGINE_VALID(v)                (((v) >> 5) & 1u)
#define NV_PTOP_ENUM_RUNLIST_VALID(v)               (((v) >> 4) & 1u)
#define NV_PTOP_TYPE_ENUM(v)                        (((v) >> 2) & 0xffu)    /* 30:2 */

#define NV_PTOP_TYPE_GRAPHICS  0u
#define NV_PTOP_TYPE_COPY0     1u
#define NV_PTOP_TYPE_COPY1     2u
#define NV_PTOP_TYPE_COPY2     3u
#define NV_PTOP_TYPE_MSPDEC    8u
#define NV_PTOP_TYPE_MSPPP     9u
#define NV_PTOP_TYPE_MSVLD    10u
#define NV_PTOP_TYPE_MSENC    11u
#define NV_PTOP_TYPE_VIC      12u
#define NV_PTOP_TYPE_SEC      13u
#define NV_PTOP_TYPE_NVENC0   14u
#define NV_PTOP_TYPE_NVENC1   15u
#define NV_PTOP_TYPE_NVDEC    16u
#define NV_PTOP_TYPE_LCE      19u
#define NV_PTOP_TYPE_GSP      20u
#define NV_PTOP_TYPE_NVJPG    21u
#define NV_PTOP_TYPE_UNKNOWN          0xffffffffu   /* no entry of this device named a type */


#endif
