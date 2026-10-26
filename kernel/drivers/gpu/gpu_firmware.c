/* The coprocessor's firmware images, held in memory and read the way the driver that boots them reads them.
 *
 * This is the half of the intake that the boot stub cannot do: the stub sees one file at a time and keeps
 * nothing, while the sequence that eventually starts a Turing-or-later GSP needs the *contents* - the payload
 * section of the RM image, and the four named sections of the FMC image whose lengths are the arguments to the
 * FSP's verify-and-boot command.  So the stub now allocates a region, copies the images into it, verifies its
 * own copy against the checksum it took off the disk, and hands the kernel a physical address.  Everything
 * below works on that copy, and everything it reports is a fact about bytes this kernel actually read: no
 * size here is quoted from a header field that was not also checked against the image's bounds.
 *
 * The rules are not invented here.  For the FMC image they are nvkm/subdev/gsp/gh100.c, verbatim in intent:
 * the first 52 bytes must equal a published template; the section table must start immediately after the
 * header; every section but the string table must be SHT_PROGBITS with flags 0xff00000a (SHF_MASKPROC |
 * SHF_MASKOS | SHF_OS_NONCONFORMING | SHF_ALLOC), the string table SHT_STRTAB with SHF_STRINGS; each section
 * must lie inside the image; a non-zero sh_info is a CRC32 of that section's bytes; and `hash', `publickey',
 * `signature' and `image' must all be present.  For the RM image they are rm/r535/gsp.c's
 * r535_gsp_elf_section(): an ELF64 whose section names come from the table at e_shstrndx, with `.fwimage' as
 * the payload and a per-chip `.fwsignature_*' section beside it.
 *
 * Provenance, stated because this kernel is its own and not a copy: no line of nouveau's or the RM's C is in
 * this file, which is written against the rules above rather than ported from them.  The one exception is the
 * 52-byte `fmc_header' template below, a data table quoted from nvkm/subdev/gsp/gh100.c (GPL-2.0), because a
 * value compared byte for byte is worth nothing if it is reconstructed from memory.
 *
 * What this does not do, in one sentence the panel repeats: no byte of any of it has been written to a card.
 * The FSP command that would consume these lengths is the next piece of work, and until it exists the honest
 * description of this file is "the OS now holds the images and can read them", not "the coprocessor can be
 * started".
 */
#include "gpu.h"
#include "scos.h"
#include "boot.h"

/* Logged separately from the module's lines, but with the same "gpu: " stem, so `log' shows the boot stub's
 * reading of the disk and this kernel's reading of the bytes side by side. */
#define FIRMWARE_LOG_PREFIX "gpu: firmware "

/* Filled by gpu_firmware_note(), which is called when the boot handoff is still in scope; read by the panel,
 * the TTY command and the notification, all of which only report what it holds. */
static struct gpu_firmware_state firmware_record;

static const char *const firmware_role_names[GPU_FIRMWARE_ROLES] = BOOT_FIRMWARE_ROLE_NAMES;

const char *gpu_firmware_role_name(unsigned role)
{
    return role < GPU_FIRMWARE_ROLES ? firmware_role_names[role] : "";
}

const struct gpu_firmware_state *gpu_firmware_state(void)
{
    return &firmware_record;
}

/* The same reflected CRC32 the disk image is checksummed with, and the same one the FMC format stores in a
 * section header: one polynomial, or the comparison means nothing. */
static uint32_t firmware_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = ~0u;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & -(int)(crc & 1));
    }
    return ~crc;
}

/* The FMC header the open driver memcmp's, byte for byte, before it will look at a section: ELFCLASS32,
 * little-endian, e_shoff 52 (the table starts where the header stops), e_ehsize 52, no program headers,
 * e_shentsize 40, six sections, string table at index 1.  Quoted from nvkm/subdev/gsp/gh100.c rather than
 * reconstructed, because a template nobody can point at is a guess with a constant next to it. */
static const uint8_t fmc_header[52] = {
    0x7f, 'E', 'L', 'F', 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    52, 0, 0, 0, 0, 0, 0, 0,
    52, 0, 0, 0,
    0, 0, 40, 0,
    6, 0, 1, 0
};

#define FMC_SHT_PROGBITS 1u
#define FMC_SHT_STRTAB   3u
#define FMC_SHF_ALLOC    2u
#define FMC_SHF_STRINGS  0x20u     /* SHF_STRINGS, the one flag the string table carries */
#define FMC_SHF_FLAGS    0xff00000au       /* SHF_MASKPROC|SHF_MASKOS|SHF_OS_NONCONFORMING|SHF_ALLOC */

static uint16_t firmware_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t firmware_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t firmware_le64(const uint8_t *p) { return (uint64_t)firmware_le32(p) | ((uint64_t)firmware_le32(p + 4) << 32); }

static int firmware_memcmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 1;
    return 0;                                       /* zero when equal, as the memcmp it stands in for */
}

static int firmware_name_is(const char *table, uint32_t room, uint32_t offset, const char *want)
{
    if (offset >= room) return 0;
    for (uint32_t i = offset; i < room; i++) {
        char a = table[i], b = *want++;
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

static int firmware_name_starts(const char *table, uint32_t room, uint32_t offset, const char *prefix)
{
    if (offset >= room) return 0;
    for (uint32_t i = 0; prefix[i]; i++)
        if (offset + i >= room || table[offset + i] != prefix[i]) return 0;
    return 1;
}

/* One image, read from the region the firmware kept.  `base + offset' is where the stub put this file and
 * `bytes' is how much of the region it owns: every read below is bounded by that window rather than by the
 * region, so a lying section header in one image cannot make this code read its neighbour's bytes and call
 * the result a fact about this one. */
static void firmware_scan_image(struct gpu_firmware_image *out, const uint8_t *base, uint32_t total,
                                uint32_t offset, uint32_t bytes)
{
    const uint8_t *image = base + offset;
    uint32_t shoff, shentsize, shnum, strndx;
    int sixty_four;

    out->resident = 1;
    if (offset > total || bytes > total - offset || bytes < 52) return;
    if (!(image[0] == 0x7f && image[1] == 'E' && image[2] == 'L' && image[3] == 'F')) return;
    if (image[4] == 2 && image[5] == 1) sixty_four = 1;
    else if (image[4] == 1 && image[5] == 1) sixty_four = 0;
    else return;

    out->offset = offset;
    out->bytes = bytes;
    if (!sixty_four) {
        /* The FMC contract first, because that is what the FSP is given: an exact header, then sections whose
         * CRC32s are stored in their own headers.  Anything that fails it is reported as failing it, not as
         * "a file we could not read". */
        out->elf32 = 1;
        out->fmc_template = !firmware_memcmp(image, fmc_header, sizeof fmc_header);
    } else
        out->elf64 = 1;

    shentsize = sixty_four ? firmware_le16(image + 58) : firmware_le16(image + 46);
    shnum = sixty_four ? firmware_le16(image + 60) : firmware_le16(image + 48);
    strndx = sixty_four ? firmware_le16(image + 62) : (image[50] | (image[51] << 8));
    shoff = (uint32_t)(sixty_four ? firmware_le64(image + 40) : firmware_le32(image + 32));
    if (!shnum || shentsize < (sixty_four ? 64u : 40u) || shoff >= bytes) return;
    if ((uint64_t)shnum * shentsize > bytes - shoff) return;
    out->sections = shnum;

    /* The string table, bounded before a single name is compared. */
    if (strndx >= shnum) return;
    {
        const uint8_t *shstr = image + shoff + (uint64_t)strndx * shentsize;
        uint32_t string_at = sixty_four ? (uint32_t)firmware_le64(shstr + 24) : firmware_le32(shstr + 16);
        uint32_t string_size = sixty_four ? (uint32_t)firmware_le64(shstr + 32) : firmware_le32(shstr + 20);
        if (string_at >= bytes || string_size > bytes - string_at) return;
        const char *strings = (const char *)(image + string_at);

        /* The FMC rules apply to the 32-bit image only; the RM image is checked by the two lookups below,
         * which is all r535_gsp_elf_section() asks of it. */
        int fmc = !sixty_four && out->fmc_template;
        uint32_t section_begin = fmc ? shoff + shnum * shentsize : 0;
        int all_sections_ok = 1, found = 0;      /* one bit per name, so a duplicate cannot overwrite the first */

        for (uint32_t i = 0; i < shnum; i++) {
            const uint8_t *shdr = image + shoff + (uint64_t)i * shentsize;
            uint32_t name = firmware_le32(shdr);
            uint32_t type = firmware_le32(shdr + 4);
            uint32_t flags = sixty_four ? (uint32_t)firmware_le64(shdr + 8) : firmware_le32(shdr + 8);
            uint32_t soffset, ssize, scrc = 0;
            if (sixty_four) {
                uint64_t o64 = firmware_le64(shdr + 24), s64 = firmware_le64(shdr + 32);
                scrc = firmware_le32(shdr + 44);            /* sh_info, 32 bits at its own offset */
                if (o64 > bytes || s64 > bytes - o64) { all_sections_ok = 0; continue; }
                soffset = (uint32_t)o64; ssize = (uint32_t)s64;
            } else {
                soffset = firmware_le32(shdr + 16); ssize = firmware_le32(shdr + 20);
                scrc = firmware_le32(shdr + 28);
                if (soffset > bytes || ssize > bytes - soffset) { all_sections_ok = 0; continue; }
            }
            if (fmc && i) {
                /* Every rule the loader applies, and the order it applies them in: a section outside the
                 * image is fatal, a wrong type or flag is fatal, and a CRC that does not match its bytes is
                 * fatal - the last being the one check here that can tell a tampered image from a merely
                 * unfamiliar one. */
                if (i == strndx) {
                    if (type != FMC_SHT_STRTAB || flags != FMC_SHF_STRINGS) all_sections_ok = 0;
                } else {
                    if (type != FMC_SHT_PROGBITS || flags != FMC_SHF_FLAGS) all_sections_ok = 0;
                    if (soffset < section_begin) all_sections_ok = 0;
                }
                if (scrc && scrc != firmware_crc32(image + soffset, ssize)) out->crc_failures++;
                /* elf_section() in the driver returns the first header whose name matches, so the first one
                 * found here is the one recorded: a file with two "image" sections describes the first to the
                 * FSP, and inventing a rule of our own about which to prefer would be a difference nobody
                 * asked for. */
                if (firmware_name_is(strings, string_size, name, "image") && !(found & 1)) {
                    out->image_offset = soffset; out->image_bytes = ssize; found |= 1;
                } else if (firmware_name_is(strings, string_size, name, "hash") && !(found & 2)) {
                    out->hash_bytes = ssize; found |= 2;
                } else if (firmware_name_is(strings, string_size, name, "publickey") && !(found & 4)) {
                    out->publickey_bytes = ssize; found |= 4;
                } else if (firmware_name_is(strings, string_size, name, "signature") && !(found & 8)) {
                    out->fmc_signature_bytes = ssize; found |= 8;
                }
            }
            if (sixty_four) {
                if (firmware_name_is(strings, string_size, name, ".fwimage")) {
                    out->payload_offset = soffset; out->payload_bytes = ssize;
                }
                if (firmware_name_starts(strings, string_size, name, ".fwsignature")) {
                    out->signature_offset = soffset; out->signature_bytes = ssize;
                    for (uint32_t c = 0; c + 1 < sizeof out->signature_name; c++) {
                        out->signature_name[c] = strings[name + c];
                        if (!out->signature_name[c]) break;
                        if (!strings[name + c]) { out->signature_name[c] = 0; break; }
                    }
                    out->signature_name[sizeof out->signature_name - 1] = 0;
                }
            }
        }
        if (fmc) {
            out->fmc_sections = all_sections_ok && !out->crc_failures ? 1 : 0;
            out->fmc_named = found == 15 ? 1 : 0;   /* all four, or the verify command has nothing to be given */
            out->section_begin = section_begin;
        }
    }
}

void gpu_firmware_scan(struct gpu_firmware_state *f, const uint8_t *base, uint32_t total)
{
    unsigned kept = 0;

    for (unsigned i = 0; i < GPU_FIRMWARE_ROLES; i++) {
        struct gpu_firmware_image *image = &f->image[i];
        const struct gpu_firmware_role *role = &f->role[i];
        *image = (struct gpu_firmware_image){0};
        if (role->offset == ~0u || !role->bytes) continue;         /* read, reported, not kept */
        firmware_scan_image(image, base, total, role->offset, role->bytes);
        /* The copy has to still be the file the stub checksummed.  If it is not, something between the
         * boot allocation and this read changed the bytes, and every number below would be a description of
         * a different image than the one on the disk. */
        image->checksum_matches = firmware_crc32(base + role->offset, role->bytes) == role->crc32 ? 1 : 0;
        kept++;
    }
    f->resident = kept;
    f->parsed = 1;
}

void gpu_firmware_note(const struct boot_handoff *handoff)
{
    struct gpu_firmware_state *f = &firmware_record;
    const struct boot_firmware *fw = &handoff->firmware;
    unsigned i, staged = 0, complete = 1;
    uint32_t bytes = 0, flags = 0;

    for (i = 0; i < GPU_FIRMWARE_ROLES; i++) {
        const struct boot_firmware_role *r = &fw->role[i];
        f->role[i].state = r->state;
        f->role[i].bytes = r->bytes;
        f->role[i].crc32 = r->crc32;
        f->role[i].flags = r->flags;
        f->role[i].payload_bytes = r->payload_bytes;
        f->role[i].signature_bytes = r->signature_bytes;
        f->role[i].sections = r->sections;
        f->role[i].offset = r->offset;
        if (r->state == BOOT_FIRMWARE_STATE_ABSENT || r->state > BOOT_FIRMWARE_STATE_UNREADABLE) {
            complete = 0;
            continue;
        }
        staged++;
        bytes += r->bytes;
        flags |= r->flags;
    }
    complete = complete && (f->role[0].flags & BOOT_FIRMWARE_ELF64)
             && (f->role[0].flags & BOOT_FIRMWARE_PAYLOAD)
             && (f->role[0].flags & BOOT_FIRMWARE_SIGNATURE)
             && !(f->role[0].flags & (BOOT_FIRMWARE_BAD_MAGIC | BOOT_FIRMWARE_TRUNCATED
                                      | BOOT_FIRMWARE_NO_STRINGS));
    for (i = 0; i < sizeof f->signature_name && fw->signature_name[i]; i++)
        f->signature_name[i] = fw->signature_name[i];
    f->signature_name[i] = 0;
    f->staged = staged;
    f->present = staged ? 1 : 0;
    f->total_bytes = bytes;
    f->flags = flags;
    f->complete = complete ? 1 : 0;
    f->consistent = (staged == fw->staged && bytes == fw->total_bytes && flags == fw->flags
                     && complete == (fw->complete ? 1u : 0u)) ? 1 : 0;
    f->address = fw->address;
    f->resident_bytes = fw->resident_bytes;
    if (fw->resident_bytes && fw->address)
        gpu_firmware_scan(f, (const uint8_t *)(uintptr_t)fw->address, fw->resident_bytes);
    if (staged) {
        klog(FIRMWARE_LOG_PREFIX "gsp firmware: %u of %u images read by the stub, %u B, set %s", staged,
             (unsigned)GPU_FIRMWARE_ROLES, bytes, complete ? "complete" : "incomplete");
        if (!f->consistent) klog(FIRMWARE_LOG_PREFIX "gsp firmware: summary disagrees with the per-file records");
        if (f->resident)
            klog(FIRMWARE_LOG_PREFIX "gsp firmware: %u image(s) held at 0x%x (%u B), parsed by this kernel",
                 f->resident, (unsigned)f->address, f->resident_bytes);
        else
            klog(FIRMWARE_LOG_PREFIX "gsp firmware: no bytes kept, so nothing beyond size and checksum is known");
    }
}
