/* RSA-2048 / SHA-256 PKCS#1 v1.5 signature verification for the UEFI stub.
 *
 * The stub is the root of trust: it carries the public modulus below and
 * refuses to load a kernel until the manifest on the ESP verifies.  This is a
 * small, fixed-size, freestanding implementation - 32 64-bit limbs, no heap,
 * no Montgomery constants to get wrong, no libc, no SSE.  Verification with
 * e=65537 costs 18 modular multiplications; on real hardware that is a few
 * milliseconds, in QEMU a fraction of a second.
 *
 * Strictness matters more than speed here.  The encoded message is checked
 * against the exact 225-byte EMSA-PKCS1-v1_5 prefix for SHA-256 (2+203+1+19),
 * so Bleichenbacher-style "e=3 / garbage after the hash" forgeries cannot
 * parse.  Only PKCS#1 v1.5 with SHA-256 and a 2048-bit modulus is accepted.
 */
#include "security.h"   /* freestanding: uint8_t/uint32_t/size_t, sc_sha256_hash */

#define LIMBS 32
#define MOD_BYTES (LIMBS * 8)

static void zzero(void *p, size_t n)
{
    uint8_t *q = (uint8_t *)p;
    while (n--) *q++ = 0;
}
static void zcopy(void *d, const void *s, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
}

static void from_be(const uint8_t b[MOD_BYTES], uint64_t x[LIMBS])
{
    unsigned i, j;
    zzero(x, sizeof(uint64_t) * LIMBS);
    for (i = 0; i < LIMBS; i++) {
        uint64_t v = 0;
        for (j = 0; j < 8; j++) v = (v << 8) | b[i * 8 + j];
        x[LIMBS - 1 - i] = v;                 /* x[0] is the least significant */
    }
}

static void to_be(const uint64_t x[LIMBS], uint8_t b[MOD_BYTES])
{
    unsigned i, j;
    for (i = 0; i < LIMBS; i++) {
        uint64_t v = x[LIMBS - 1 - i];
        for (j = 0; j < 8; j++) b[i * 8 + j] = (uint8_t)(v >> (56 - 8 * j));
    }
}

/* r has LIMBS+1 limbs; n has LIMBS.  r < 2n is an invariant of the loop. */
static int ge_n(const uint64_t r[LIMBS + 1], const uint64_t n[LIMBS])
{
    int i;
    if (r[LIMBS]) return 1;
    for (i = LIMBS - 1; i >= 0; i--) {
        if (r[i] > n[i]) return 1;
        if (r[i] < n[i]) return 0;
    }
    return 1;
}

static void sub_n(uint64_t r[LIMBS + 1], const uint64_t n[LIMBS])
{
    unsigned i;
    unsigned __int128 borrow = 0;
    for (i = 0; i < LIMBS; i++) {
        unsigned __int128 t = (unsigned __int128)r[i] - n[i] - borrow;
        r[i] = (uint64_t)t;
        borrow = (t >> 64) & 1;   /* wrapped => the subtraction borrowed */
    }
    r[LIMBS] -= (uint64_t)borrow;
}

/* out = a*b mod n, all little-endian limb arrays.  The full 4096-bit product
 * is reduced by shift-and-conditionally-subtract; one subtraction per bit is
 * enough because the remainder is kept below n throughout. */
static void mulmod(const uint64_t a[LIMBS], const uint64_t b[LIMBS],
                   const uint64_t n[LIMBS], uint64_t out[LIMBS])
{
    uint64_t p[2 * LIMBS], r[LIMBS + 1];
    unsigned i, j;
    int bit, top;
    zzero(p, sizeof(p));
    for (i = 0; i < LIMBS; i++) {
        unsigned __int128 carry = 0;
        for (j = 0; j < LIMBS; j++) {
            unsigned __int128 t = (unsigned __int128)a[i] * b[j] + p[i + j] + carry;
            p[i + j] = (uint64_t)t;
            carry = t >> 64;
        }
        p[i + LIMBS] = (uint64_t)carry;
    }
    zzero(r, sizeof(r));
    top = -1;
    for (i = 0; i < 2 * LIMBS; i++) if (p[i]) top = (int)i;
    if (top < 0) { zzero(out, sizeof(uint64_t) * LIMBS); return; }
    for (bit = top * 64 + 63; bit >= 0; bit--) {
        uint64_t carry = (p[(unsigned)bit / 64] >> ((unsigned)bit % 64)) & 1;
        for (i = 0; i <= LIMBS; i++) {
            uint64_t next = r[i] >> 63;
            r[i] = (r[i] << 1) | carry;
            carry = next;
        }
        if (ge_n(r, n)) sub_n(r, n);
    }
    for (i = 0; i < LIMBS; i++) out[i] = r[i];
    zzero(p, sizeof(p));
    zzero(r, sizeof(r));
}

/* EMSA-PKCS1-v1_5 encoding of a SHA-256 digest for a 2048-bit modulus:
 * 00 01 || ff*202 || 00 || DigestInfo(SHA-256) || digest.
 * emLen(256) - tLen(51) - 3 = 202 padding bytes; the DER prefix is 19 bytes. */
static int encoded_ok(const uint8_t em[MOD_BYTES], const uint8_t digest[32])
{
    static const uint8_t sha256_info[19] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
        0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
    };
    unsigned i, bad = 0;
    bad |= em[0] ^ 0x00;
    bad |= em[1] ^ 0x01;
    for (i = 0; i < 202; i++) bad |= em[2 + i] ^ 0xff;
    bad |= em[204] ^ 0x00;
    for (i = 0; i < 19; i++) bad |= em[205 + i] ^ sha256_info[i];
    for (i = 0; i < 32; i++) bad |= em[224 + i] ^ digest[i];
    return bad == 0;
}

static int cmp_be(const uint8_t a[MOD_BYTES], const uint8_t b[MOD_BYTES])
{
    unsigned i;
    for (i = 0; i < MOD_BYTES; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

int boot_rsa_verify(const uint8_t *msg, size_t len,
                    const uint8_t sig[MOD_BYTES],
                    const uint8_t pub_n[MOD_BYTES])
{
    uint64_t base[LIMBS], acc[LIMBS], n[LIMBS], tmp[LIMBS];
    uint8_t em[MOD_BYTES], digest[32];
    int bit, ok;
    if (!pub_n || !sig || (!msg && len)) return 0;
    if (!(pub_n[0] & 0x80)) return 0;          /* must be a full 2048-bit modulus */
    if (cmp_be(sig, pub_n) >= 0) return 0;     /* signature representative out of range */
    sc_sha256_hash(msg, len, digest);
    from_be(pub_n, n);
    from_be(sig, base);
    zzero(acc, sizeof(acc));
    acc[0] = 1;
    /* e = 65537 = 2^16 + 1: square for each bit, multiply where set. */
    for (bit = 16; bit >= 0; bit--) {
        mulmod(acc, acc, n, tmp); zcopy(acc, tmp, sizeof(acc));
        if ((65537u >> bit) & 1u) {
            mulmod(acc, base, n, tmp); zcopy(acc, tmp, sizeof(acc));
        }
    }
    to_be(acc, em);
    ok = encoded_ok(em, digest);
    zzero(base, sizeof(base)); zzero(acc, sizeof(acc));
    zzero(n, sizeof(n)); zzero(tmp, sizeof(tmp));
    zzero(em, sizeof(em));
    return ok;
}
