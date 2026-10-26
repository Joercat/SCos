/* See gsp_fw.h for what these files are, why these five names, and what "examined" does and does not
 * claim.  This file is compiled twice: into BOOTX64.EFI, where it runs before the kernel exists and on the
 * machine whose card is in question, and into the host test binary, where every branch is driven with a
 * hand-made blob and the bounds are checked under ASan.  It is deliberately free of any dependency on
 * either environment: no allocator, no libc, no firmware calls, and no write to anything. */
#include "gsp_fw.h"

/* The names, kept next to each other so a reader can see the pairing the comment above describes. */
static const char *const role_names[BOOT_FIRMWARE_ROLES] = BOOT_FIRMWARE_ROLE_NAMES;
static const char *const role_files[BOOT_FIRMWARE_ROLES] = BOOT_FIRMWARE_ROLE_FILES;

const char *gsp_fw_role_name(unsigned role)
{
    return role < BOOT_FIRMWARE_ROLES ? role_names[role] : "";
}

const char *gsp_fw_role_file(unsigned role)
{
    return role < BOOT_FIRMWARE_ROLES ? role_files[role] : "";
}

uint32_t gsp_fw_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = ~0u;
    while (length--) {
        crc ^= *bytes++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(int)(crc & 1));
    }
    return ~crc;
}

/* Prefix test, used only for the signature section: a GB20x image is expected to carry
 * `.fwsignature_gb20x' and a different package a different suffix, and the name found is reported so the
 * reader can see which one this file is instead of being told the one that was looked for. */
static int name_starts_with(const char *table, size_t room, uint32_t offset, const char *prefix)
{
    if ((size_t)offset >= room) return 0;
    for (size_t i = 0; prefix[i]; i++)
        if ((size_t)offset + i >= room || table[offset + i] != prefix[i]) return 0;
    return 1;
}

static void copy_name(char *dst, size_t room, const char *table, size_t limit, uint32_t offset)
{
    if (!dst || !room) return;
    size_t n = 0;
    for (; n + 1 < room && (size_t)offset + n < limit && table[offset + n]; n++) dst[n] = table[offset + n];
    dst[n] = 0;
}

static int name_is(const char *table, size_t room, uint32_t offset, const char *want)
{
    if ((size_t)offset >= room) return 0;           /* the name would start outside the string table */
    for (size_t i = offset; i < room; i++) {
        char a = table[i], b = want[i - offset];
        if (a != b) return 0;                       /* differing byte, including one side ending earlier */
        if (!a) return 1;                           /* both ended here: the same name */
    }
    return 0;                                       /* the table ran out before the name did */
}

/* ELF64 field offsets, spelled out because a freestanding boot application includes no <elf.h> and because
 * the values below are the ones the driver whose loader this mirrors uses: e_ident magic 0..3, EI_CLASS 4,
 * EI_DATA 5, e_shoff 40, e_shentsize 58, e_shnum 60, e_shstrndx 62; a section header is name 0, offset 24,
 * size 32, and 64 bytes long.  Reading them by hand instead of through a struct is what keeps the layout
 * independent of this compiler's padding rules. */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

void gsp_fw_examine(const void *data, size_t length, struct boot_firmware_role *out,
                    char *sig_name, size_t sig_room)
{
    const uint8_t *base = data;

    if (sig_name && sig_room) sig_name[0] = 0;

    out->flags = 0;
    out->payload_bytes = out->signature_bytes = 0;
    out->sections = 0;
    if (!data || length < 64) { out->flags |= BOOT_FIRMWARE_BAD_MAGIC; return; }
    if (!(base[0] == 0x7f && base[1] == 'E' && base[2] == 'L' && base[3] == 'F')) {
        out->flags |= BOOT_FIRMWARE_BAD_MAGIC;
        return;
    }
    /* Class and byte order are the two things that make the rest of the walk meaningful; the machine field
     * is deliberately not checked, because the firmware image is not an x86 binary and the driver this
     * mirrors does not look at it either. */
    if (base[4] != 2 || base[5] != 1) { out->flags |= BOOT_FIRMWARE_BAD_MAGIC; return; }

    uint64_t shoff = le64(base + 40);
    uint32_t shentsize = le16(base + 58), shnum = le16(base + 60), strndx = le16(base + 62);
    if (shentsize != 64 || !shnum) { out->flags |= BOOT_FIRMWARE_TRUNCATED; return; }
    if (shoff > (uint64_t)length || shnum > (uint64_t)(length - (size_t)shoff) / 64ull) {
        out->flags |= BOOT_FIRMWARE_TRUNCATED;
        return;
    }
    out->flags |= BOOT_FIRMWARE_ELF64;
    if (strndx >= shnum) { out->flags |= BOOT_FIRMWARE_NO_STRINGS; return; }

    const uint8_t *table = base + (size_t)shoff;
    const uint8_t *string_header = table + (size_t)strndx * 64ull;
    uint64_t string_offset = le64(string_header + 24), string_size = le64(string_header + 32);
    if (string_offset > (uint64_t)length || string_size > (uint64_t)length - string_offset) {
        out->flags |= BOOT_FIRMWARE_NO_STRINGS;
        return;
    }
    const char *strings = (const char *)(base + (size_t)string_offset);

    /* Every section is walked, not just the two wanted: the count is reported, so "a container with no
     * sections at all" and "a container whose sections did not include the one we need" are different
     * answers in the panel, and on a card where the next step depends on which it is they have to be. */
    for (uint32_t i = 0; i < shnum; i++) {
        const uint8_t *shdr = table + (uint64_t)i * 64ull;
        uint32_t name = le32(shdr);
        uint64_t offset = le64(shdr + 24), size = le64(shdr + 32);
        out->sections++;
        if (offset > (uint64_t)length || size > (uint64_t)length - offset) {
            out->flags |= BOOT_FIRMWARE_TRUNCATED;         /* a section the file cannot hold: stop trusting it */
            continue;
        }
        if (name_is(strings, (size_t)string_size, name, ".fwimage")) {
            out->flags |= BOOT_FIRMWARE_PAYLOAD;
            out->payload_bytes = (uint32_t)size;
        }
        if (name_starts_with(strings, (size_t)string_size, name, ".fwsignature")) {
            out->flags |= BOOT_FIRMWARE_SIGNATURE;
            out->signature_bytes = (uint32_t)size;
            copy_name(sig_name, sig_room, strings, (size_t)string_size, name);
        }
    }
}
