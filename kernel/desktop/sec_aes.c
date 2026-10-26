/* SCos AES-256 (FIPS 197) in CTR mode (SP 800-38A) plus an
 * encrypt-then-MAC AEAD built from HMAC-SHA256 (RFC 2104).
 *
 * Self-contained: no tables that depend on host byte order, no libc, no SSE.
 * The same object is compiled for the kernel (-mgeneral-regs-only) and for the
 * host known-answer tests.  Constant-time with respect to secret data on this
 * CPU: the S-box is a fixed public table; the AEAD compares tags with
 * sc_ct_eq() and refuses to release plaintext before the tag verifies.
 *
 * Key separation: sc_aead_seal/open derive independent 256-bit encryption and
 * MAC subkeys from the master key with labelled HMACs, so the same VEK can
 * safely encrypt the filesystem and authenticate its header.
 */
#include "security.h"

#define AES_ROUNDS 14
#define AES_WORDS  60
#define AES_BLOCK  16

static const uint8_t SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

struct aes256 {
    uint32_t rk[AES_WORDS];
};

static uint32_t word_of(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t rotword(uint32_t w)
{
    return (w << 8) | (w >> 24);
}

static uint32_t subword(uint32_t w)
{
    return ((uint32_t)SBOX[(w >> 24) & 255] << 24) |
           ((uint32_t)SBOX[(w >> 16) & 255] << 16) |
           ((uint32_t)SBOX[(w >> 8) & 255] << 8) |
           (uint32_t)SBOX[w & 255];
}

static void aes256_init(struct aes256 *a, const uint8_t key[32])
{
    uint32_t *w = a->rk;
    static const uint8_t rcon[7] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40 };
    int i;
    for (i = 0; i < 8; i++) w[i] = word_of(key + 4 * i);
    for (i = 8; i < AES_WORDS; i++) {
        uint32_t t = w[i-1];
        if ((i & 7) == 0) t = subword(rotword(t)) ^ ((uint32_t)rcon[i/8 - 1] << 24);
        else if ((i & 7) == 4) t = subword(t);
        w[i] = w[i-8] ^ t;
    }
}

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((uint8_t)(x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static void add_round_key(uint8_t s[16], const uint32_t *rk, int round)
{
    for (int c = 0; c < 4; c++) {
        uint32_t w = rk[round * 4 + c];
        s[4*c+0] ^= (uint8_t)(w >> 24);
        s[4*c+1] ^= (uint8_t)(w >> 16);
        s[4*c+2] ^= (uint8_t)(w >> 8);
        s[4*c+3] ^= (uint8_t)w;
    }
}

static void sub_shift(uint8_t s[16])
{
    uint8_t t[16];
    /* State is column-major: s[4*c+r]. ShiftRows moves row r left by r. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[4*c+r] = SBOX[s[4*((c+r) & 3)+r]];
    for (int i = 0; i < 16; i++) s[i] = t[i];
}

static void mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = s + 4*c;
        uint8_t a0=p[0], a1=p[1], a2=p[2], a3=p[3];
        uint8_t all = a0 ^ a1 ^ a2 ^ a3;
        p[0] ^= all ^ xtime((uint8_t)(a0 ^ a1));
        p[1] ^= all ^ xtime((uint8_t)(a1 ^ a2));
        p[2] ^= all ^ xtime((uint8_t)(a2 ^ a3));
        p[3] ^= all ^ xtime((uint8_t)(a3 ^ a0));
    }
}

void sc_aes256_encrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16])
{
    struct aes256 a;
    uint8_t s[16];
    int r;
    aes256_init(&a, key);
    for (int i = 0; i < 16; i++) s[i] = in[i];
    add_round_key(s, a.rk, 0);
    for (r = 1; r < AES_ROUNDS; r++) {
        sub_shift(s);
        mix_columns(s);
        add_round_key(s, a.rk, r);
    }
    sub_shift(s);
    add_round_key(s, a.rk, AES_ROUNDS);
    for (int i = 0; i < 16; i++) out[i] = s[i];
    sc_burn(&a, sizeof(a));
    sc_burn(s, sizeof(s));
}

/* 128-bit big-endian counter increment over the whole IV block.  A fresh
 * random IV per message (vault save) means counter values never repeat. */
static void ctr_inc(uint8_t c[16])
{
    for (int i = 15; i >= 0; i--) if (++c[i]) break;
}

void sc_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                   uint8_t *out, const uint8_t *in, size_t len)
{
    uint8_t counter[16], stream[16];
    for (int i = 0; i < 16; i++) counter[i] = iv[i];
    while (len) {
        size_t n = len < 16 ? len : 16;
        size_t i;
        sc_aes256_encrypt_block(key, counter, stream);
        for (i = 0; i < n; i++) out[i] = in ? in[i] ^ stream[i] : stream[i];
        out += n; if (in) in += n; len -= n;
        ctr_inc(counter);
        if (len == 0) break;
    }
    sc_burn(counter, sizeof(counter));
    sc_burn(stream, sizeof(stream));
}

static void derive_subkeys(const uint8_t master[32], uint8_t enc[32], uint8_t mac[32])
{
    static const uint8_t le[] = "SCos-AES256-CTR-enc";
    static const uint8_t lm[] = "SCos-HMAC-SHA256-mac";
    sc_hmac_hash(master, 32, le, sizeof(le) - 1, enc);
    sc_hmac_hash(master, 32, lm, sizeof(lm) - 1, mac);
}

static void aead_tag(const uint8_t mac[32], const uint8_t iv[16],
                     const uint8_t *ad, size_t adlen,
                     const uint8_t *ct, size_t ctlen, uint8_t tag[32])
{
    uint8_t lenbuf[8];
    sc_hmac h;
    sc_hmac_init(&h, mac, 32);
    sc_hmac_update(&h, "SCos-AEAD-v1", 12);
    /* The nonce is authenticated, not just the ciphertext: otherwise an
     * attacker can change the IV and make the same tag verify against a
     * different plaintext. */
    sc_hmac_update(&h, iv, 16);
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)((uint64_t)adlen >> (56 - 8*i));
    sc_hmac_update(&h, lenbuf, 8);
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)((uint64_t)ctlen >> (56 - 8*i));
    sc_hmac_update(&h, lenbuf, 8);
    if (ad && adlen) sc_hmac_update(&h, ad, adlen);
    if (ct && ctlen) sc_hmac_update(&h, ct, ctlen);
    sc_hmac_final(&h, tag);
    sc_burn(&h, sizeof(h));
}

void sc_aead_seal(const uint8_t key[32], const uint8_t iv[16],
                  const uint8_t *ad, size_t adlen,
                  const uint8_t *pt, size_t ptlen,
                  uint8_t *ct, uint8_t tag[32])
{
    uint8_t enc[32], mac[32];
    derive_subkeys(key, enc, mac);
    sc_aes256_ctr(enc, iv, ct, pt, ptlen);
    aead_tag(mac, iv, ad, adlen, ct, ptlen, tag);
    sc_burn(enc, sizeof(enc));
    sc_burn(mac, sizeof(mac));
}

int sc_aead_open(const uint8_t key[32], const uint8_t iv[16],
                 const uint8_t *ad, size_t adlen,
                 const uint8_t *ct, size_t ctlen,
                 uint8_t *pt, const uint8_t tag[32])
{
    uint8_t enc[32], mac[32], want[32];
    int ok;
    derive_subkeys(key, enc, mac);
    aead_tag(mac, iv, ad, adlen, ct, ctlen, want);
    ok = sc_ct_eq(want, tag, 32);
    if (ok) sc_aes256_ctr(enc, iv, pt, ct, ctlen);
    else if (pt && ctlen) for (size_t i = 0; i < ctlen; i++) pt[i] = 0;
    sc_burn(enc, sizeof(enc));
    sc_burn(mac, sizeof(mac));
    sc_burn(want, sizeof(want));
    return ok;
}

/* Wrap/unwrap a 32-byte volume key with a password-derived KEK.  The nonce is
 * the user's random KDF salt (regenerated on every password change), so the
 * CTR keystream is never reused with the same KEK and nonce. */
/* Separate the wrap key into an encryption key and a MAC key.  Using the same
 * 32 bytes as both an AES key and an HMAC key is a needless cross-protocol
 * reuse: the labelled HMAC derivation costs two compressions and removes it. */
static void vek_subkeys(const uint8_t kek[32], uint8_t enc[32], uint8_t mac[32])
{
    static const uint8_t le[] = "SCos-VEK-wrap-enc-v2";
    static const uint8_t lm[] = "SCos-VEK-wrap-mac-v2";
    sc_hmac_hash(kek, 32, le, sizeof(le) - 1, enc);
    sc_hmac_hash(kek, 32, lm, sizeof(lm) - 1, mac);
}

void sc_vek_wrap(const uint8_t kek[32], const uint8_t nonce[16],
                 const uint8_t vek[32], uint8_t wrapped[48])
{
    uint8_t enc[32], mac[32], tag[32];
    sc_hmac h;
    vek_subkeys(kek, enc, mac);
    sc_aes256_ctr(enc, nonce, wrapped, vek, 32);
    sc_hmac_init(&h, mac, 32);
    sc_hmac_update(&h, "SCos-VEK-wrap-v1|", 17);
    sc_hmac_update(&h, nonce, 16);
    sc_hmac_update(&h, wrapped, 32);
    sc_hmac_final(&h, tag);
    for (int i = 0; i < 16; i++) wrapped[32 + i] = tag[i];
    sc_burn(&h, sizeof(h));
    sc_burn(enc, sizeof(enc)); sc_burn(mac, sizeof(mac));
    sc_burn(tag, sizeof(tag));
}

int sc_vek_unwrap(const uint8_t kek[32], const uint8_t nonce[16],
                  const uint8_t wrapped[48], uint8_t vek[32])
{
    uint8_t enc[32], mac[32], tag[32];
    sc_hmac h;
    int ok;
    vek_subkeys(kek, enc, mac);
    sc_hmac_init(&h, mac, 32);
    sc_hmac_update(&h, "SCos-VEK-wrap-v1|", 17);
    sc_hmac_update(&h, nonce, 16);
    sc_hmac_update(&h, wrapped, 32);
    sc_hmac_final(&h, tag);
    ok = sc_ct_eq(tag, wrapped + 32, 16);
    if (ok) sc_aes256_ctr(enc, nonce, vek, wrapped, 32);
    else for (int i = 0; i < 32; i++) vek[i] = 0;
    sc_burn(&h, sizeof(h));
    sc_burn(enc, sizeof(enc)); sc_burn(mac, sizeof(mac));
    sc_burn(tag, sizeof(tag));
    return ok;
}
