/* The host-side half of the coprocessor-firmware test: gsp_fw.c, compiled the way the boot stub compiles it,
 * run against containers built here byte by byte.
 *
 * What this proves and what it does not.  It proves the walk accepts the container format the open
 * driver's own loader reads (ELF64, section table, named sections) and refuses every malformed
 * variant without reading outside the buffer - the malformed variants are the interesting ones,
 * because the input is a file somebody copied onto a stick and the code runs before any OS exists to
 * catch a fault.  It does not prove that a real 570.144 blob parses, since no blob is distributed
 * here; that part can only be measured on the machine that has the files, which is what the panel line
 * is for.
 *
 * Built with ASan and UBSan by tools/tests/test_gsp_firmware.py: an out-of-bounds read here is a failure of
 * the run, not a note.
 */
#include <stdio.h>
#include <string.h>
#include "../../boot/uefi/gsp_fw.c"

static int checks, failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) { failures++; printf("FAIL: %s\n", what); }
}

static void check_eq(unsigned got, unsigned want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL: %s (got %u want %u)\n", what, got, want);
    }
}

static void put16(unsigned char *p, unsigned value) { p[0] = value & 0xff; p[1] = value >> 8; }
static void put32(unsigned char *p, uint32_t value)
{
    put16(p, value & 0xffff); put16(p + 2, value >> 16);
}
static void put64(unsigned char *p, uint64_t value) { put32(p, (uint32_t)value); put32(p + 4, (uint32_t)(value >> 32)); }

/* A container holding the sections a GSP image is expected to carry, built field by field because the
 * interesting cases are all "one field of the header lies": each argument below exists to make exactly one
 * of them wrong.  Everything lives in `storage', which the sanitizer knows the size of. */
static unsigned char storage[8192];
static const char strtab[] = "\0.fwimage\0.fwsignature_gb20x\0.shstrtab";

static uint32_t string_offset(const char *want)
{
    for (size_t i = 0; i < sizeof strtab - 1; i++)
        if (strcmp(strtab + i, want) == 0) return (uint32_t)i;
    return 0xffffffffu;
}

struct section { uint32_t name; uint64_t offset, size; };

/* `shentsize' 0 means 64, the only size a 64-bit section header has. */
static size_t build_elf(int with_fwimage, int with_signature, uint32_t shentsize, uint32_t shnum_override,
                        int lie_shoff_past_end, int lie_strtab_outside)
{
    struct section list[4];
    unsigned count = 0;
    size_t at = 64, table;

    memset(storage, 0, sizeof storage);
    memcpy(storage, "\x7f" "ELF", 4);
    storage[4] = 2;                                   /* ELFCLASS64 */
    storage[5] = 1;                                   /* ELFDATA2LSB */
    storage[6] = 1;                                   /* EV_CURRENT */

    list[count++] = (struct section){ 0, 0, 0 };      /* the index-0 null section, as the format requires */
    if (with_fwimage) {
        list[count] = (struct section){ string_offset(".fwimage"), at, 1024 };
        memset(storage + at, 0xa5, 1024);
        at += 1024; count++;
    }
    if (with_signature) {
        list[count] = (struct section){ string_offset(".fwsignature_gb20x"), at, 512 };
        memset(storage + at, 0x5a, 512);
        at += 512; count++;
    }
    list[count] = (struct section){ string_offset(".shstrtab"), at, sizeof strtab - 1 };
    memcpy(storage + at, strtab, sizeof strtab - 1);
    at += sizeof strtab - 1; count++;

    table = (at + 7) & ~(size_t)7;
    for (unsigned i = 0; i < count; i++) {
        unsigned char *h = storage + table + (size_t)i * 64;
        put32(h + 0, list[i].name);
        put32(h + 4, i == 0 ? 0 : 1);                 /* SHT_PROGBITS, whatever the walk is asked about */
        put64(h + 24, list[i].offset);
        put64(h + 32, list[i].size);
    }
    put64(storage + 40, lie_shoff_past_end ? 0xfffff000u : table);
    put16(storage + 52, 64);                          /* e_ehsize */
    put16(storage + 58, shentsize ? (uint16_t)shentsize : 64);
    put16(storage + 60, shnum_override ? (uint16_t)shnum_override : (uint16_t)count);
    put16(storage + 62, lie_strtab_outside ? (uint16_t)(count + 4) : (uint16_t)(count - 1));
    return table + (size_t)count * 64;
}

int main(int argc, char **argv)
{
    char sig[32];
    struct boot_firmware_role r;

    /* Second mode: examine a container somebody else built, and assert it holds what the boot sequence
     * reads out of it.  tools/makedisk.py is fed a file from tools/tests/test_gsp_firmware.py, which builds
     * its own containers in python rather than calling this code - so without this mode, the two builders can
     * describe different shapes, and the first anyone finds out is a panel that says "no .fwimage section"
     * for a file that has one.  That is exactly what happened once, which is why the mode exists. */
    if (argc > 2 && strcmp(argv[1], "--check") == 0) {
        static unsigned char staged[1 << 16];
        FILE *f = fopen(argv[2], "rb");
        if (!f) { printf("FAIL: cannot read %s\n", argv[2]); return 1; }
        size_t n = fread(staged, 1, sizeof staged, f);
        fclose(f);
        memset(&r, 0, sizeof r); sig[0] = 0;
        gsp_fw_examine(staged, n, &r, sig, sizeof sig);
        check_eq(r.flags & (BOOT_FIRMWARE_ELF64 | BOOT_FIRMWARE_PAYLOAD | BOOT_FIRMWARE_SIGNATURE),
                 BOOT_FIRMWARE_ELF64 | BOOT_FIRMWARE_PAYLOAD | BOOT_FIRMWARE_SIGNATURE,
                 "the test container holds both sections a boot sequence asks for");
        check_eq(r.payload_bytes, 1024, "payload size read from the section header");
        check_eq(r.signature_bytes, 512, "signature size read from the section header");
        check_eq(r.sections, 4, "four section headers, as the builder laid them out");
        check_eq(r.flags & (BOOT_FIRMWARE_BAD_MAGIC | BOOT_FIRMWARE_TRUNCATED | BOOT_FIRMWARE_NO_STRINGS),
                 0, "and no failure bit, which is what makes a complete set complete");
        check(strcmp(sig, ".fwsignature_gb20x") == 0, "the signature section is reported by name");
        printf("%d checks, %d failures (container %s, %u B)\n", checks, failures, argv[2], (unsigned)n);
        return failures ? 1 : 0;
    }

    /* The checksum has to be the one every `crc32' utility prints, or the panel number is decoration:
     * the standard check value for "123456789" is 0xCBF43926. */
    check(gsp_fw_crc32("123456789", 9) == 0xcbf43926u, "crc32 is the standard polynomial, not a local one");

    /* A complete container: the two sections the boot sequence reads, by name, with their sizes. */
    {
        size_t length = build_elf(1, 1, 0, 0, 0, 0);
        memset(&r, 0, sizeof r); sig[0] = 0;
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_ELF64 ? 1 : 0, 1, "a 64-bit little-endian container is walkable");
        check_eq(r.flags & BOOT_FIRMWARE_PAYLOAD ? 1 : 0, 1, "the payload section is found by name");
        check_eq(r.flags & BOOT_FIRMWARE_SIGNATURE ? 1 : 0, 1, "the signature section is found, prefix-matched");
        check_eq(r.payload_bytes, 1024, "and its size reported, not assumed");
        check_eq(r.signature_bytes, 512, "as is the signature section size");
        check(strcmp(sig, ".fwsignature_gb20x") == 0, "the signature section is reported by its own name");
        check_eq(r.sections, 4, "every section header was walked, including the ones nobody asked about");
        check(!(r.flags & (BOOT_FIRMWARE_BAD_MAGIC | BOOT_FIRMWARE_TRUNCATED | BOOT_FIRMWARE_NO_STRINGS)),
              "a well-formed container earns no failure bit");
    }

    /* A payload with no signature: what the driver would report as "section not found", reported here as
     * the same distinction rather than as a single "bad file". */
    {
        size_t length = build_elf(1, 0, 0, 0, 0, 0);
        memset(&r, 0, sizeof r); sig[0] = 0;
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_PAYLOAD ? 1 : 0, 1, "payload still found");
        check_eq(r.flags & BOOT_FIRMWARE_SIGNATURE ? 1 : 0, 0, "a set with no signature section says so");
        check(sig[0] == 0, "and names none");
    }

    /* The other way round: nothing is invented for a missing payload. */
    {
        size_t length = build_elf(0, 1, 0, 0, 0, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_PAYLOAD ? 1 : 0, 0, "no payload, no claim of one");
        check_eq(r.payload_bytes, 0, "the size stays zero rather than guessing");
    }

    /* Every malformed header below has to be *reported*, not fatal, and not read out of bounds. */
    {
        size_t length = build_elf(1, 1, 0, 0, 0, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine("a text file, not an image", 23, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_BAD_MAGIC ? 1 : 0, 1, "a file with no ELF magic is not half-parsed");
        check_eq(r.sections, 0, "and no section is claimed to exist");

        storage[4] = 1;                             /* ELFCLASS32 */
        length = build_elf(1, 1, 0, 0, 0, 0); storage[4] = 1;
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_BAD_MAGIC ? 1 : 0, 1, "a 32-bit container is refused, not walked");
        storage[4] = 2;

        length = build_elf(1, 1, 0, 0, 0, 0); storage[5] = 2;   /* big-endian */
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_BAD_MAGIC ? 1 : 0, 1, "big-endian is refused for the same reason");
    }

    /* The header lies about where the table is, how big its entries are, and how many there are.  These are
     * the cases a hostile or merely truncated file produces, and the ones an out-of-bounds read lives in. */
    {
        size_t length = build_elf(1, 1, 0, 0, 1, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_TRUNCATED ? 1 : 0, 1, "a section table past the end is truncated");
        check_eq(r.flags & BOOT_FIRMWARE_ELF64 ? 1 : 0, 0, "and the file earns no walkable mark");

        length = build_elf(1, 1, 0, 4000, 0, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_TRUNCATED ? 1 : 0, 1, "a count the file cannot hold is caught by size");
        check_eq(r.sections, 0, "rather than by reading whatever follows");

        length = build_elf(1, 1, 48, 0, 0, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_TRUNCATED ? 1 : 0, 1, "an entry size other than 64 stops the walk");

        length = build_elf(1, 1, 0, 0, 0, 1);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_NO_STRINGS ? 1 : 0, 1, "a string table index outside the table is named");
        check_eq(r.flags & BOOT_FIRMWARE_ELF64 ? 1 : 0, 1, "the header is still a real one, though");
    }

    /* A name that starts inside the string table but does not end there, and a name index outside it: both
     * have to fail to match instead of running off the end of the buffer. */
    {
        size_t length = build_elf(1, 1, 0, 0, 0, 0);
        unsigned char *table = storage + (size_t)le64(storage + 40);
        put32(table + 0, 0xffffffffu);              /* the null section points its name far away */
        put32(table + 64 + 0, (uint32_t)length * 2u);   /* and so does the one holding the payload */
        memset(&r, 0, sizeof r); sig[0] = 0;
        gsp_fw_examine(storage, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_NO_STRINGS ? 1 : 0, 0, "a name outside the table is a miss, not a fault");
        check_eq(r.flags & BOOT_FIRMWARE_TRUNCATED ? 1 : 0, 0, "and is not counted as damage to the file");
        check_eq(r.sections, 4, "the walk still visits every header it was given");
        check_eq(r.flags & BOOT_FIRMWARE_SIGNATURE ? 1 : 0, 1, "the sections whose names do resolve are matched");
    }

    /* A truncated file is the most likely real-world damage of all - an incomplete copy - and the shortest
     * one is a one-byte file, which must not even be looked at. */
    {
        size_t length = build_elf(1, 1, 0, 0, 0, 0);
        for (size_t cut = 1; cut < 64; cut += 7) {
            memset(&r, 0, sizeof r);
            gsp_fw_examine(storage, cut, &r, sig, sizeof sig);
            check_eq(r.flags & BOOT_FIRMWARE_BAD_MAGIC ? 1 : 0, 1, "a file shorter than a header is not a container");
        }
        memset(&r, 0, sizeof r);
        gsp_fw_examine(0, 0, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_BAD_MAGIC ? 1 : 0, 1, "no bytes at all is answered, not crashed");
        (void)length;
    }

    /* The signature name is copied into a caller's buffer of the caller's size; a two-byte buffer has to
     * hold two bytes and a terminator and nothing more. */
    {
        char tiny[3] = { 'z', 'z', 'z' };
        size_t length = build_elf(1, 1, 0, 0, 0, 0);
        memset(&r, 0, sizeof r);
        gsp_fw_examine(storage, length, &r, tiny, sizeof tiny);
        check_eq(r.flags & BOOT_FIRMWARE_SIGNATURE ? 1 : 0, 1, "the section is still found with a small buffer");
        check(tiny[2] == 0, "the name is cut to the room given, with room left for the terminator");
        check(memcmp(tiny, ".\0", 1) == 0, "and starts from the beginning, as a truncation should");
    }

    /* The role table is what the whole intake is named by: five roles, five 8.3 files, in this order.  The
     * same two lists are duplicated in tools/makedisk.py and quoted in the panel text, and the python half
     * of this suite fails if they drift; the numbers are pinned here so a rename cannot be half-done. */
    check_eq(BOOT_FIRMWARE_ROLES, 5, "five roles, as the open driver names them");
    check(strcmp(gsp_fw_role_name(0), "gsp") == 0, "role 0 is the RM image");
    check(strcmp(gsp_fw_role_name(1), "fmc") == 0, "role 1 is the FMC");
    check(strcmp(gsp_fw_role_name(2), "bootloader") == 0, "role 2 is the bootloader");
    check(strcmp(gsp_fw_role_name(3), "booter load") == 0, "role 3 is the booter load image");
    check(strcmp(gsp_fw_role_name(4), "booter unload") == 0, "role 4 is the booter unload image");
    check(strcmp(gsp_fw_role_file(0), "GSP.BIN") == 0, "role 0 is read from GSP.BIN");
    check(strcmp(gsp_fw_role_file(1), "GSPFMC.BIN") == 0, "role 1 from GSPFMC.BIN");
    check(strcmp(gsp_fw_role_file(2), "GSPBL.BIN") == 0, "role 2 from GSPBL.BIN");
    check(strcmp(gsp_fw_role_file(3), "GSPBTL.BIN") == 0, "role 3 from GSPBTL.BIN");
    check(strcmp(gsp_fw_role_file(4), "GSPBTU.BIN") == 0, "role 4 from GSPBTU.BIN");
    check(gsp_fw_role_name(9)[0] == 0 && gsp_fw_role_file(9)[0] == 0, "a role outside the set names nothing");
    for (unsigned i = 0; i < BOOT_FIRMWARE_ROLES; i++) {
        const char *f = gsp_fw_role_file(i);
        check(strlen(f) <= 12, "every ESP name fits an 8.3 slot");
        check(strchr(f, '.') == f + strlen(f) - 4, "and exactly one dot, before BIN");
        for (const char *c = f; *c; c++)
            check(*c == '.' || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'),
                  "no lower case and no separator a FAT volume would not store");
    }

    /* The one case a bounds check earns its keep: an unterminated name at the very end of the file.  The
     * wanted string matches byte for byte up to the last byte the file holds, so a walk that compares until
     * it finds a terminator reads past the end - and this is a file somebody copied, running in code that
     * executes before any kernel exists to catch the fault.  A container whose string table simply does not
     * terminate inside the image is therefore required to answer "no match", not "no crash". */
    {
        unsigned char *e = storage;
        size_t length = 256 + 9;
        memset(storage, 0, sizeof storage);
        memcpy(e, "\x7f" "ELF", 4);
        e[4] = 2; e[5] = 1; e[6] = 1;
        put64(e + 40, 64);                          /* e_shoff */
        put16(e + 58, 64);                          /* e_shentsize */
        put16(e + 60, 3);                           /* e_shnum */
        put16(e + 62, 2);                           /* e_shstrndx: the table is the third header */
        put32(e + 64 + 0, 0);                       /* section 0: the null header */
        put32(e + 128 + 0, 1);                      /* section 1 names itself from offset 1 ... */
        put64(e + 128 + 24, 0); put64(e + 128 + 32, 64);   /* ... and holds a payload that is in the file */
        put64(e + 192 + 24, 256); put64(e + 192 + 32, 9);  /* section 2 is the string table: 9 bytes, no NUL */
        memcpy(storage + 256, "\0" ".fwimage", 9);
        memset(&r, 0, sizeof r); sig[0] = 0;
        gsp_fw_examine(e, length, &r, sig, sizeof sig);
        check_eq(r.flags & BOOT_FIRMWARE_ELF64 ? 1 : 0, 1, "the header itself is still sound");
        check_eq(r.flags & BOOT_FIRMWARE_PAYLOAD ? 1 : 0, 0, "an unterminated name is not a match");
        check_eq(r.signature_bytes, 0, "and not a signature section either");
        check_eq(r.sections, 3, "every header is still counted, because the walk never left the buffer");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
