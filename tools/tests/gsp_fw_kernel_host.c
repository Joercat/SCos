/* Host-side tests for the kernel's own reading of the coprocessor's firmware images: the shape rules
 * nvkm/subdev/gsp/gh100.c applies to an FMC image, and the two names r535_gsp_oneinit() looks up in the RM
 * image.  kernel/drivers/gpu/gpu_firmware.c is compiled straight in below, so what is tested is the file the
 * kernel builds, including its bounds checks - the point of testing here rather than on hardware is that a
 * section placed outside its image has to be refused without ever reading the byte past the end, which is
 * only observable under a sanitizer.
 *
 * The images are built to the layout the driver's template and section rules require; the sizes chosen are the
 * ones the checks assert, so a builder that put a section somewhere else fails here rather than agreeing with
 * itself.  The same code is run over files the QEMU test builds, in --fmc and --rm modes, which is what keeps
 * the two builders from drifting apart.
 *
 *   cc -ffreestanding -fno-builtin -fsanitize=address,undefined -Ikernel/include \
 *      tools/tests/gsp_fw_kernel_host.c -o /tmp/gspk && /tmp/gspk
 *
 * The kernel source is pulled in by #include rather than compiled alongside, because the rules under test are
 * file-static; -ffreestanding -fno-builtin matches how the kernel builds, and is also what stops the host's
 * <string.h> from disagreeing with the kernel's own declarations of the same names.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "boot.h"

void klog(const char *fmt, ...) { (void)fmt; }        /* gpu_firmware_note() logs; nothing here calls it */

#include "../../kernel/drivers/gpu/gpu_firmware.c"

/* What to break in a built image, so each failure can be attributed to one rule. */
#define TEST_NO_CRC 1u       /* leave the per-section CRC32 out, which the driver allows */
#define TEST_BAD_STRTAB 2u   /* point e_shstrndx at something that is not a string table */
#define TEST_OUTSIDE 1u      /* a section that reaches past the end of the file */

static unsigned checks_run;
static unsigned failures;

static void check(const char *what, int ok)
{
    checks_run++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* An FMC image exactly as gh100.c wants it: the 52-byte template header verbatim, then six 40-byte section
 * headers at 52, then a string table, hash, publickey, signature and image. */
#define FMC_HASH 32u
#define FMC_PKEY 4u
#define FMC_SIG 256u
#define FMC_IMAGE 512u

static uint32_t build_fmc(uint8_t *p, uint32_t capacity, unsigned opts)
{
    static const uint8_t strings[] = "\0hash\0publickey\0signature\0image\0";
    uint32_t shoff = 52, shdrs = 6 * 40, strings_at = 52 + shdrs, strings_size = sizeof(strings);
    uint32_t at = strings_at + strings_size;
    struct { const char *name; uint32_t size; } section[] = {
        { "hash", FMC_HASH }, { "publickey", FMC_PKEY }, { "signature", FMC_SIG }, { "image", FMC_IMAGE }
    };

    if (capacity < 52 + shdrs + strings_size + FMC_HASH + FMC_PKEY + FMC_SIG + FMC_IMAGE) return 0;
    memset(p, 0, capacity);
    memcpy(p, fmc_header, sizeof fmc_header);
    memcpy(p + strings_at, strings, strings_size);          /* the names themselves: headers alone name nothing */

    for (unsigned i = 0; i < 4; i++) {                 /* string table first: it is what names the rest */
        uint32_t name = 0;
        for (uint32_t k = 1; k < strings_size; k++)
            if (!memcmp(&strings[k], section[i].name, strlen(section[i].name) + 1)) { name = k; break; }
        uint8_t *shdr = p + shoff + 40 * (i + 2);
        put32(shdr, name);
        put32(shdr + 4, 1);                            /* SHT_PROGBITS */
        put32(shdr + 8, 0xff00000au);                   /* the flags the FMC contract asks for */
        put32(shdr + 16, at);
        put32(shdr + 20, section[i].size);
        memset(p + at, (uint8_t)(0x40 + i), section[i].size);   /* the bytes first: the CRC is of them */
        if (!(opts & TEST_NO_CRC))
            put32(shdr + 28, firmware_crc32(p + at, section[i].size));
        at += section[i].size;
    }
    uint8_t *shdr = p + shoff + 40;                    /* the string table itself, index 1 */
    /* sh_name stays 0 - the empty name, as in the published template.  A string table that named itself "hash"
     * would be a builder inventing a shape the format does not have, and the parser's name lookup would count
     * it as a second match. */
    put32(shdr + 4, (opts & TEST_BAD_STRTAB) ? 1u : 3u);   /* SHT_STRTAB, or deliberately not */
    put32(shdr + 8, 0x20u);                            /* SHF_STRINGS */
    put32(shdr + 16, strings_at);
    put32(shdr + 20, strings_size);
    return at;
}

static uint32_t build_rm(uint8_t *p, uint32_t capacity, unsigned opts)
{
    static const uint8_t strings[] = "\0.shstrtab\0.fwimage\0.fwsignature_gb20x\0";
    uint32_t shoff = 64, shdrs = 4 * 64, strings_at = 64 + shdrs, strings_size = sizeof(strings);
    uint32_t payload_at = strings_at + strings_size, payload = 1024, signature_at = payload_at + payload;
    uint32_t signature = 512;

    if (capacity < signature_at + signature) return 0;
    memset(p, 0, capacity);
    memcpy(p, "\177ELF\2\1\1", 7);
    put16(p + 16, 2);                                  /* ET_EXEC, as the RM image is */
    put16(p + 18, 0x3e);                               /* x86-64: never checked, and not meaningful */
    put64(p + 40, shoff);
    put16(p + 58, 64);
    put16(p + 60, 4);
    put16(p + 62, 1);
    memcpy(p + strings_at, strings, strings_size);     /* the names, not just the header that points at them */
    put32(p + shoff + 4, 3);                           /* section 1: the string table */
    put64(p + shoff + 64 + 24, strings_at);
    put64(p + shoff + 64 + 32, strings_size);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t name = 0;
        const char *want = i ? ".fwsignature_gb20x" : ".fwimage";
        for (uint32_t k = 1; k < strings_size; k++)
            if (!memcmp(&strings[k], want, strlen(want) + 1)) { name = k; break; }
        uint8_t *shdr = p + shoff + 64 * (i + 2);
        put32(shdr, name);
        put32(shdr + 4, 1);                            /* SHT_PROGBITS */
        put64(shdr + 24, i ? signature_at : payload_at);
        put64(shdr + 32, i ? signature : payload);
    }
    memset(p + payload_at, 0xa5, payload);
    memset(p + signature_at, 0x5a, signature);
    if (opts & TEST_OUTSIDE) put64(p + shoff + 64 * 2 + 24, capacity + 4096);
    return signature_at + signature;
}

static uint8_t copy_area[1u << 20];                   /* scratch for the rebuilt variants, one image at a time */

static void run_fmc(void)
{
    uint32_t capacity = 1u << 20;
    uint8_t *region = calloc(1, capacity);
    uint32_t length = build_fmc(region, capacity, 0);
    struct gpu_firmware_state state, second;
    struct gpu_firmware_image *image, *other;

    check("the FMC image the builder was asked to make is the size its sections add up to",
          length == 52 + 6 * 40 + sizeof("\0hash\0publickey\0signature\0image\0") + FMC_HASH + FMC_PKEY +
              FMC_SIG + FMC_IMAGE);
    memset(&state, 0, sizeof(state));
    state.role[1].state = BOOT_FIRMWARE_STATE_STAGED;
    state.role[1].flags = BOOT_FIRMWARE_ELF32 | BOOT_FIRMWARE_FMC_IMAGE;
    state.role[1].bytes = length;
    state.role[1].crc32 = firmware_crc32(region, length) ;
    state.role[1].offset = 0;
    state.resident = 1;
    state.address = (uintptr_t)region;
    gpu_firmware_scan(&state, region, capacity);
    image = &state.image[1];
    check("the held image is recorded as resident", image->resident);
    check("and as a 32-bit container", image->elf32 && !image->elf64);
    check("with the FMC header byte for byte", image->fmc_template);
    check("all six section headers were walked, the unused zeroth one included", image->sections == 6);
    check("every section is inside the image, named, and CRC-correct", image->fmc_sections && image->fmc_named);
    check("no CRC32 disagreed", image->crc_failures == 0);
    check("image, hash, publickey and signature were found at their offsets",
          image->image_offset == 52 + 6 * 40 + sizeof("\0hash\0publickey\0signature\0image\0") + FMC_HASH +
              FMC_PKEY + FMC_SIG &&
          image->image_bytes == FMC_IMAGE && image->hash_bytes == FMC_HASH &&
          image->publickey_bytes == FMC_PKEY && image->fmc_signature_bytes == FMC_SIG);
    check("the whole-image CRC32 matches what the boot stub recorded", image->checksum_matches);
    check("the other four roles had nothing resident, so nothing is claimed about them",
          !state.image[0].resident && !state.image[2].resident);

    /* Now the ways an image can fail, one at a time, so each is attributable. */
    uint32_t nocrc = build_fmc(copy_area, capacity, TEST_NO_CRC);
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = nocrc;
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy_area, capacity);
    other = &second.image[1];
    check("sections with no CRC32 recorded are accepted, because the driver accepts their absence",
          other->fmc_sections && other->fmc_named && other->crc_failures == 0);

    uint32_t badstrings = build_fmc(copy_area, capacity, TEST_BAD_STRTAB);
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = badstrings;
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy_area, capacity);
    check("and e_shstrndx pointing somewhere that is not a string table rejects the container, while the "
          "names still readable in it are reported as read",
          second.image[1].elf32 && !second.image[1].fmc_sections && second.image[1].fmc_named);

    uint8_t *copy = copy_area;                         /* the variants are built and broken in one scratch buffer */

    memcpy(copy, region, capacity);
    copy[18] = 0x02;                                   /* e_machine: the published template has it zero, and
                                                          nothing in the parse looks at it, so this is the one
                                                          way to be a legal image with a header that is not
                                                          the FMC one */
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = length;
    second.role[1].crc32 = firmware_crc32(copy, length) ;
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy, capacity);
    other = &second.image[1];
    check("a header that differs from the FMC template is reported, and stops the format claims with it",
          !other->fmc_template && other->elf32 && other->sections == 6 && !other->fmc_sections &&
          !other->fmc_named && other->crc_failures == 0);
    check("even though the sections are individually intact, nothing is vouched for once the header differs",
          other->hash_bytes == 0 && other->image_bytes == 0 && other->fmc_signature_bytes == 0);

    memcpy(copy, region, capacity);
    copy[52 + 6 * 40 + sizeof("\0hash\0publickey\0signature\0image\0") + 4] ^= 0xff; /* inside "hash" */
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = length;
    second.role[1].crc32 = firmware_crc32(copy, length) ;
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy, capacity);
    other = &second.image[1];
    check("a section whose bytes do not hash to its recorded CRC32 is counted and rejects the image",
          other->crc_failures == 1 && !other->fmc_sections && other->fmc_named);

    memcpy(copy, region, capacity);
    put32(copy + 52 + 40 * 5 + 20, 1u << 24);           /* "image" size, past the end of the image */
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = length;
    second.role[1].crc32 = firmware_crc32(copy, length) ;
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy, capacity);
    other = &second.image[1];
    check("a section reaching outside its own image is refused without reading it",
          !other->fmc_sections && other->image_bytes == 0 && other->fmc_named == 0);

    memcpy(copy, region, capacity);
    copy[1] = 'Z';                                     /* bytes that are not an ELF file at all */
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = length;
    second.role[1].crc32 = state.role[1].crc32;        /* the disk copy's own checksum, not the memory's: */
                                                   /* this case is about the two disagreeing */
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy, capacity);
    check("bytes that are not a container are reported as resident and nothing more",
          second.image[1].resident && !second.image[1].elf32 && !second.image[1].elf64);
    check("and it is the mismatched whole-image checksum, before any of it is read, that says so",
          !second.image[1].checksum_matches);

    memcpy(copy, region, capacity);
    memset(&second, 0, sizeof(second));
    second.resident = second.role[1].state = 1;
    second.role[1].bytes = 40;                         /* less than a container header */
    second.role[1].offset = 0;
    gpu_firmware_scan(&second, copy, 40);
    check("an image too short to hold a header is resident without being a container",
          second.image[1].resident && !second.image[1].elf32 && !second.image[1].sections);

    /* The role the boot stub could not copy: offset ~0u means "read, not kept". */
    memset(&second, 0, sizeof(second));
    second.resident = 0;
    second.role[3].state = BOOT_FIRMWARE_STATE_STAGED;
    second.role[3].bytes = length;
    second.role[3].offset = 0xffffffffu;
    gpu_firmware_scan(&second, region, capacity);
    check("a file the stub read but could not keep is not parsed, and is counted as not held",
          !second.image[3].resident && second.resident == 0 && !second.image[3].sections);
    free(region);
}

static void run_rm(void)
{
    uint32_t capacity = 1u << 20;
    uint8_t *region = calloc(1, capacity);
    uint32_t length = build_rm(region, capacity, 0);
    struct gpu_firmware_state state;
    struct gpu_firmware_image *image;

    memset(&state, 0, sizeof(state));
    state.resident = state.role[0].state = 1;
    state.role[0].flags = BOOT_FIRMWARE_ELF64 | BOOT_FIRMWARE_PAYLOAD | BOOT_FIRMWARE_SIGNATURE;
    state.role[0].bytes = length;
    state.role[0].crc32 = firmware_crc32(region, length) ;
    state.role[0].offset = 0;
    state.address = (uintptr_t)region;
    gpu_firmware_scan(&state, region, capacity);
    image = &state.image[0];
    check("the 64-bit RM image is read as one", image->resident && image->elf64 && image->sections == 4);
    check("its .fwimage is found with its size", image->payload_bytes == 1024);
    check("and its signature section by prefix, name included",
          image->signature_bytes == 512 && !strcmp(image->signature_name, ".fwsignature_gb20x"));
    check("the FMC rules are not applied to it", !image->fmc_template && !image->fmc_sections);

    build_rm(region, capacity, TEST_OUTSIDE);
    memset(&state, 0, sizeof(state));
    state.resident = state.role[0].state = 1;
    state.role[0].bytes = length;
    state.role[0].offset = 0;
    gpu_firmware_scan(&state, region, capacity);
    check("a .fwimage reaching past the end of its image is not reported as a payload",
          state.image[0].elf64 && state.image[0].payload_bytes == 0);
    free(region);
}

static int run_file(const char *mode, const char *path, unsigned role)
{
    FILE *f = fopen(path, "rb");
    uint32_t capacity = 1u << 24;
    uint8_t *region = malloc(capacity);
    size_t got;
    struct gpu_firmware_state state;
    struct gpu_firmware_image *image;
    int ok = 1;

    if (!f) { printf("cannot open %s\n", path); free(region); return 1; }
    got = fread(region, 1, capacity, f);
    fclose(f);
    memset(&state, 0, sizeof(state));
    state.resident = state.role[role].state = 1;
    state.role[role].bytes = (uint32_t)got;
    state.role[role].crc32 = firmware_crc32(region, (uint32_t)got) ;
    state.role[role].offset = 0;
    state.address = (uintptr_t)region;
    gpu_firmware_scan(&state, region, (uint32_t)got);
    image = &state.image[role];
    if (!strcmp(mode, "--fmc")) {
        ok = ok && image->elf32 && image->fmc_template && image->fmc_sections && image->fmc_named &&
             image->hash_bytes == FMC_HASH && image->publickey_bytes == FMC_PKEY &&
             image->fmc_signature_bytes == FMC_SIG && image->image_bytes == FMC_IMAGE &&
             image->crc_failures == 0 && image->checksum_matches;
    } else {
        ok = ok && image->elf64 && image->payload_bytes == 1024 && image->signature_bytes == 512 &&
             !strcmp(image->signature_name, ".fwsignature_gb20x");
    }
    printf("%s %s: %s\n", mode, path, ok ? "as the format requires" : "NOT as the format requires");
    free(region);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && (!strcmp(argv[1], "--fmc") || !strcmp(argv[1], "--rm")))
        return run_file(argv[1], argv[2], argv[1][2] == 'f' ? 1u : 0u);
    run_fmc();
    run_rm();
    printf("%u check(s), %u failure(s)\n", checks_run, failures);
    return failures != 0;
}
