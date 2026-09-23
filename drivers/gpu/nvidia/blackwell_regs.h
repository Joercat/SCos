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

#endif
