/* The firmware images the coprocessor on Turing-and-later NVIDIA chips is booted from, and the check this
 * boot stub runs on them before handing anything to the kernel.
 *
 * Why these five names and no others: they are the files the open-source Linux driver loads.  Its module
 * declarations (drivers/gpu/drm/nouveau/nvkm/subdev/gsp/priv.h, MIT, read 2026-09-24) are
 *
 *   MODULE_FIRMWARE("nvidia/" #chip "/gsp/gsp-" #vers ".bin")
 *   MODULE_FIRMWARE("nvidia/" #chip "/gsp/fmc-" #vers ".bin")
 *   MODULE_FIRMWARE("nvidia/" #chip "/gsp/bootloader-" #vers ".bin")
 *   MODULE_FIRMWARE("nvidia/" #chip "/gsp/booter_load-" #vers ".bin")
 *   MODULE_FIRMWARE("nvidia/" #chip "/gsp/booter_unload-" #vers ".bin")
 *
 * and for GB207 - the chip in the machine this was written for - nouveau's own table names version 570.144
 * and the signature section `.fwsignature_gb20x' (nvkm/subdev/gsp/gb202.c, which lists gb202..gb207 under one
 * firmware set).  A FAT32 boot volume cannot carry those paths, so each role gets one 8.3 name in \SCOS\,
 * below.  Nothing is renamed by accident: the role decides which file has to be which, and the report says
 * which role is missing.
 *
 * What "checked" means here, and what it does not.  The package holds two container shapes, and this reads
 * both, because a check written for one of them and pointed at the other reports a legitimate file as garbage.
 * The RM image is ELF64: r535_gsp.c's `r535_gsp_elf_section' walks the section table from e_shoff, resolves
 * the names through the string table at e_shstrndx, and takes `.fwimage' as the payload plus the chip's
 * signature section as the thing the falcon verifies.  The FMC image is ELF32 and is judged by other rules
 * (gh100.c's `elf_validate_sections': a 52-byte header compared byte for byte against a published template,
 * then a rule per section).  So this checks that a file is a container of the class its role implies and, for
 * a 64-bit one, that a payload section is in it and what it calls its signature section; the 32-bit image's
 * own contract - template, per-section CRC32s, the four names - is read in the kernel, where the bytes are
 * kept, and not here.  An earlier revision required ELF64 of every role and reported a real FMC image as
 * "no container"; that sentence was believed for a day before it was measured.
 * It is not a signature check, and no claim of one is made anywhere in SCos: the AES/ECDSA blobs inside
 * those sections are verified *by the card*, which is the only party that can.  A file that passes here is
 * "shaped like what the boot sequence asks for", which is a weaker and more honest statement than "valid".
 */
#ifndef SCOS_BOOT_GSP_FW_H
#define SCOS_BOOT_GSP_FW_H
#include <stdint.h>
#include <stddef.h>
#include "boot.h"        /* the states, the flags and the record shape live there, so both sides cannot mean
                          * different things by the same bit - and the host test reads the same header */

/* The record each role produces is `struct boot_firmware_role' in kernel/include/boot.h, along with its
 * states (ABSENT / STAGED / TOO_BIG / UNREADABLE) and its flags (ELF64, ELF32, PAYLOAD, SIGNATURE, the four
 * FMC bits and the three failure bits).  Those names appear nowhere in this header on purpose: the kernel reads the same numbers
 * out of the handoff, and one vocabulary is the only way the two sides cannot drift apart. */

/* The five roles, in the order the report names them.  Both the stub and tools/makedisk.py use these
 * strings, and the host test fails if the two ever drift. */
const char *gsp_fw_role_name(unsigned role);       /* "gsp", "fmc", ... - what the panel prints */
const char *gsp_fw_role_file(unsigned role);       /* "GSP.BIN", ... - what sits on the ESP */

uint32_t gsp_fw_crc32(const void *data, size_t length);   /* the polynomial the kernel image is checked with */

/* Examine one staged file: which container it is, whether it holds a `.fwimage', and whether it holds a
 * signature section at all.  The signature section's *name* is reported rather than matched against a
 * per-chip expectation, because the stub has no chip table to expect with and because "this blob carries
 * .fwsignature_gb20x" is a fact about the file, while "this blob is for a GB207" would be an inference from
 * a name.  Never reads outside [data,data+length); `sig_name' may be NULL.  It writes the fields a caller
 * has not already filled - flags, payload and signature sizes, section count - and leaves state, bytes and
 * crc32 to whoever read the file. */
void gsp_fw_examine(const void *data, size_t length, struct boot_firmware_role *out,
                    char *sig_name, size_t sig_room);

#endif
