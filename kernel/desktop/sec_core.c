/* SCos security core: cryptographic primitives, credential policy and the
 * account-record codec.  Pure C11 with no kernel or libc dependencies, so the
 * same object is linked into the kernel and into tools/tests/security_host.c.
 *
 * Everything here is deliberately small and auditable.  SHA-256 is FIPS 180-4,
 * HMAC is RFC 2104, PBKDF2 is RFC 8018 (HMAC-SHA256, one output block).
 */
#include "security.h"

/* ---------------------------------------------------------- small strings --- */
size_t sc_len(const char *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }

int sc_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int sc_neq(const char *a, const char *b, size_t n)
{
    if (!a || !b) return 1;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

char *sc_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return dst;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

char *sc_cat(char *dst, size_t cap, const char *src)
{
    size_t i = sc_len(dst);
    if (i >= cap) return dst;
    sc_copy(dst + i, cap - i, src);
    return dst;
}

char *sc_cpy(char *dst, const char *src)
{
    size_t i = 0;
    if (src) for (; src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

void sc_fmt(char *dst, size_t cap, uint64_t v)
{
    char tmp[24];
    size_t n = 0;
    if (!cap) return;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (n > cap - 1) n = cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = tmp[n - i - 1];
    dst[n] = 0;
}

int sc_num(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        if (v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10) return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    if (out) *out = v;
    return 1;
}

int sc_find(const char *hay, const char *needle)
{
    size_t n;
    if (!hay || !needle) return 0;
    n = sc_len(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] == needle[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static char lower_of(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

void sc_lower(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = lower_of(src[i]);
    dst[i] = 0;
}

/* --------------------------------------------------------------- SHA-256 --- */
static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t load32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void compress(sc_sha256 *c, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2;
    unsigned i;
    for (i = 0; i < 16; i++) w[i] = load32(block + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K256[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sc_sha256_init(sc_sha256 *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->used = 0; c->total = 0;
    for (unsigned i = 0; i < 64; i++) c->block[i] = 0;
}

void sc_sha256_update(sc_sha256 *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p) return;
    c->total += len;
    while (len) {
        size_t take = 64 - c->used;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) c->block[c->used + i] = p[i];
        c->used += take; p += take; len -= take;
        if (c->used == 64) { compress(c, c->block); c->used = 0; }
    }
}

void sc_sha256_final(sc_sha256 *c, uint8_t out[SC_HASH_BYTES])
{
    uint64_t bits = c->total * 8;
    uint8_t pad[72];
    size_t n = (c->used < 56) ? (56 - c->used) : (120 - c->used);
    for (size_t i = 0; i < sizeof(pad); i++) pad[i] = 0;
    pad[0] = 0x80;
    for (size_t i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - i * 8));
    sc_sha256_update(c, pad, n + 8);
    for (unsigned i = 0; i < 8; i++) be32(out + i * 4, c->h[i]);
}

void sc_sha256_hash(const void *data, size_t len, uint8_t out[SC_HASH_BYTES])
{
    sc_sha256 c;
    sc_sha256_init(&c);
    sc_sha256_update(&c, data, len);
    sc_sha256_final(&c, out);
}

/* ------------------------------------------------------------------ HMAC --- */
void sc_hmac_init(sc_hmac *c, const void *key, size_t keylen)
{
    uint8_t k[64], hashed[SC_HASH_BYTES], ipad[64], opad[64];
    const uint8_t *src = (const uint8_t *)key;
    size_t i;
    for (i = 0; i < sizeof(k); i++) k[i] = 0;
    if (keylen > 64) {
        sc_sha256_hash(src, keylen, hashed);
        for (i = 0; i < SC_HASH_BYTES; i++) k[i] = hashed[i];
        keylen = SC_HASH_BYTES;
    } else if (src) {
        for (i = 0; i < keylen; i++) k[i] = src[i];
    }
    for (i = 0; i < 64; i++) { ipad[i] = (uint8_t)(k[i] ^ 0x36); opad[i] = (uint8_t)(k[i] ^ 0x5c); }
    sc_sha256_init(&c->inner); sc_sha256_update(&c->inner, ipad, 64);
    sc_sha256_init(&c->outer); sc_sha256_update(&c->outer, opad, 64);
    sc_burn(k, sizeof(k)); sc_burn(hashed, sizeof(hashed));
    sc_burn(ipad, sizeof(ipad)); sc_burn(opad, sizeof(opad));
}

void sc_hmac_update(sc_hmac *c, const void *data, size_t len)
{
    sc_sha256_update(&c->inner, data, len);
}

void sc_hmac_final(const sc_hmac *c, uint8_t out[SC_HASH_BYTES])
{
    uint8_t inner[SC_HASH_BYTES];
    sc_sha256 a = c->inner, b = c->outer;
    sc_sha256_final(&a, inner);
    sc_sha256_update(&b, inner, SC_HASH_BYTES);
    sc_sha256_final(&b, out);
    sc_burn(inner, sizeof(inner));
}

void sc_hmac_hash(const void *key, size_t keylen, const void *msg, size_t mlen,
                  uint8_t out[SC_HASH_BYTES])
{
    sc_hmac c;
    sc_hmac_init(&c, key, keylen);
    sc_hmac_update(&c, msg, mlen);
    sc_hmac_final(&c, out);
    sc_burn(&c, sizeof(c));
}

void sc_pbkdf2_blocks(const void *pw, size_t pwlen,
                      const uint8_t *salt, size_t saltlen,
                      uint32_t iterations, uint32_t blocks, uint8_t *out)
{
    sc_hmac base, round;
    uint8_t u[SC_HASH_BYTES], acc[SC_HASH_BYTES], counter[4];
    uint32_t block, i;
    if (!iterations) iterations = 1;
    if (!blocks || !out) return;
    sc_hmac_init(&base, pw, pwlen);
    for (block = 1; block <= blocks; block++) {
        counter[0] = (uint8_t)(block >> 24);
        counter[1] = (uint8_t)(block >> 16);
        counter[2] = (uint8_t)(block >> 8);
        counter[3] = (uint8_t)block;
        round = base;
        sc_hmac_update(&round, salt, saltlen);
        sc_hmac_update(&round, counter, 4);
        sc_hmac_final(&round, u);
        for (i = 0; i < SC_HASH_BYTES; i++) acc[i] = u[i];
        for (i = 1; i < iterations; i++) {
            unsigned j;
            round = base;
            sc_hmac_update(&round, u, SC_HASH_BYTES);
            sc_hmac_final(&round, u);
            for (j = 0; j < SC_HASH_BYTES; j++) acc[j] ^= u[j];
        }
        for (i = 0; i < SC_HASH_BYTES; i++)
            out[(block - 1) * SC_HASH_BYTES + i] = acc[i];
    }
    sc_burn(&base, sizeof(base)); sc_burn(&round, sizeof(round));
    sc_burn(u, sizeof(u)); sc_burn(acc, sizeof(acc));
}

void sc_pbkdf2(const void *pw, size_t pwlen,
               const uint8_t *salt, size_t saltlen,
               uint32_t iterations, uint8_t out[SC_HASH_BYTES])
{
    sc_pbkdf2_blocks(pw, pwlen, salt, saltlen, iterations, 1, out);
}

int sc_ct_eq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff = (uint8_t)(diff | (x[i] ^ y[i]));
    return diff == 0;
}

void sc_burn(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/* ------------------------------------------------------------------- hex --- */
void sc_hex(char *out, size_t cap, const uint8_t *in, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (!cap) return;
    for (i = 0; i < n && i * 2 + 2 < cap; i++) {
        out[i*2] = digits[in[i] >> 4];
        out[i*2+1] = digits[in[i] & 15];
    }
    out[i*2] = 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sc_unhex(uint8_t *out, size_t cap, const char *in, size_t hexchars)
{
    size_t i;
    if (!in || (hexchars & 1) || hexchars / 2 > cap) return 1;
    for (i = 0; i < hexchars; i += 2) {
        int a = hex_nibble(in[i]), b = hex_nibble(in[i+1]);
        if (a < 0 || b < 0) return 1;
        out[i/2] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

void sc_db_digest(const char *body, size_t len, char out[65])
{
    uint8_t d[SC_HASH_BYTES];
    sc_sha256_hash(body, len, d);
    sc_hex(out, 65, d, SC_HASH_BYTES);
}

int sc_db_verify(const char *data, size_t len)
{
    uint8_t want[SC_HASH_BYTES], got[SC_HASH_BYTES];
    size_t pos = 0, body_end = 0;
    int found = 0;
    if (!data || len < 20 || len > SC_DB_MAX) return 0;
    while (pos < len) {
        size_t e = pos;
        while (e < len && data[e] != '\n') e++;
        if (e == pos) return 0;                     /* blank line */
        if (data[pos] == 'C' && e - pos > 2 && data[pos+1] == '|') {
            size_t hlen = e - pos - 2;
            if (found || hlen != 64) return 0;
            if (sc_unhex(want, sizeof(want), data + pos + 2, hlen)) return 0;
            body_end = pos;
            found = 1;
            pos = (e < len) ? e + 1 : e;
            if (pos != len) return 0;               /* C| must be the last line */
            break;
        }
        pos = (e < len) ? e + 1 : e;
    }
    if (!found || body_end < 10) return 0;
    sc_sha256_hash(data, body_end, got);
    return sc_ct_eq(got, want, SC_HASH_BYTES);
}

/* ---------------------------------------------------------------- policy --- */
void sc_policy_default(struct sc_policy *p)
{
    /* Hardened defaults for full-security builds.  Existing databases keep
     * their own recorded values, so tightening these never locks anyone out. */
    p->min_len = 12; p->min_classes = 3; p->min_bits = 50;
    p->max_fails = 5; p->lock_base = 30; p->lock_max = 900;
    p->idle_minutes = 10; p->max_age_days = 90; p->history = 5;
    p->iters = 200000;
}

int sc_policy_valid(const struct sc_policy *p)
{
    if (p->min_len < 8 || p->min_len > 64) return 0;
    if (p->min_classes < 1 || p->min_classes > 4) return 0;
    if (p->min_bits > 128) return 0;
    if (p->max_fails < 1 || p->max_fails > 20) return 0;
    if (p->lock_base < 5 || p->lock_base > 3600) return 0;
    if (p->lock_max < p->lock_base || p->lock_max > 86400) return 0;
    if (p->idle_minutes > 1440 || p->max_age_days > 3650) return 0;
    if (p->history > SC_HISTORY_MAX) return 0;
    if (p->iters < 10000 || p->iters > 2000000) return 0;
    return 1;
}

static char *emit(char *o, char *end, const char *s)
{
    while (*s) {
        if (o >= end) return end;
        *o++ = *s++;
    }
    *o = 0;
    return o;
}

static char *emit_num(char *o, char *end, uint64_t v)
{
    char t[24];
    sc_fmt(t, sizeof(t), v);
    return emit(o, end, t);
}

int sc_policy_encode(const struct sc_policy *p, char *out, size_t cap)
{
    char *o, *end;
    if (cap < 32) return -1;
    o = out; end = out + cap - 1;
    o = emit(o, end, "P|");
    o = emit_num(o, end, p->min_len);      o = emit(o, end, "|");
    o = emit_num(o, end, p->min_classes);  o = emit(o, end, "|");
    o = emit_num(o, end, p->min_bits);     o = emit(o, end, "|");
    o = emit_num(o, end, p->max_fails);    o = emit(o, end, "|");
    o = emit_num(o, end, p->lock_base);    o = emit(o, end, "|");
    o = emit_num(o, end, p->lock_max);     o = emit(o, end, "|");
    o = emit_num(o, end, p->idle_minutes); o = emit(o, end, "|");
    o = emit_num(o, end, p->max_age_days); o = emit(o, end, "|");
    o = emit_num(o, end, p->history);      o = emit(o, end, "|");
    o = emit_num(o, end, p->iters);
    if (o >= end) return -1;
    *o++ = '\n'; *o = 0;
    return (int)(o - out);
}

/* Split on `sep'.  A trailing separator still denotes a (empty) final field:
 * an account with no password history ends "…|0|" and that last empty string
 * is f[15].  Dropping it makes a freshly created database fail to parse after
 * the first reboot, which is exactly the failure this regression guards. */
static int split_fields(char *line, char sep, char **f, int max)
{
    int n = 0;
    char *p = line;
    if (!*p) return 0;
    for (;;) {
        if (n >= max) return -1;
        f[n++] = p;
        while (*p && *p != sep) p++;
        if (!*p) break;                  /* last field */
        *p++ = 0;
        if (!*p) {                       /* trailing separator */
            if (n >= max) return -1;
            f[n++] = p;
            break;
        }
    }
    return n;
}

static int field_u32(const char *s, uint32_t *out)
{
    uint64_t v;
    if (!sc_num(s, &v) || v > 0xffffffffULL) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int field_u64(const char *s, uint64_t *out)
{
    return sc_num(s, out);
}

int sc_policy_decode(const char *line, struct sc_policy *p)
{
    /* "P|" + min_len, min_classes, min_bits, max_fails, lock_base, lock_max,
     * idle_minutes, max_age_days, history, iters = 11 pipe-separated fields. */
    char buf[256], *f[11];
    int n;
    if (!line || sc_len(line) >= sizeof(buf)) return 0;
    sc_cpy(buf, line);
    n = split_fields(buf, '|', f, 11);
    if (n != 11 || !sc_eq(f[0], "P")) return 0;
    if (!field_u32(f[1], &p->min_len) || !field_u32(f[2], &p->min_classes) ||
        !field_u32(f[3], &p->min_bits) || !field_u32(f[4], &p->max_fails) ||
        !field_u32(f[5], &p->lock_base) || !field_u32(f[6], &p->lock_max) ||
        !field_u32(f[7], &p->idle_minutes) || !field_u32(f[8], &p->max_age_days) ||
        !field_u32(f[9], &p->history) || !field_u32(f[10], &p->iters)) return 0;
    return sc_policy_valid(p);
}

/* ----------------------------------------------------------- account codec --- */
int sc_account_encode(const struct sc_account *a, char *out, size_t cap)
{
    char salt[SC_SALT_BYTES*2+1], hash[SC_HASH_BYTES*2+1], hist[SC_HISTORY_MAX*96+1];
    char *o, *end;
    if (cap < 64) return -1;
    o = out; end = out + cap - 1;
    sc_hex(salt, sizeof(salt), a->salt, SC_SALT_BYTES);
    sc_hex(hash, sizeof(hash), a->hash, SC_HASH_BYTES);
    sc_hex(hist, sizeof(hist), &a->hist[0][0],
           (size_t)a->hist_count * (SC_SALT_BYTES + SC_HASH_BYTES));
    o = emit(o, end, "U|");
    o = emit_num(o, end, a->uid);         o = emit(o, end, "|");
    o = emit(o, end, a->name);            o = emit(o, end, "|");
    o = emit(o, end, a->display);         o = emit(o, end, "|");
    o = emit_num(o, end, a->role);        o = emit(o, end, "|");
    o = emit_num(o, end, a->flags);       o = emit(o, end, "|");
    o = emit_num(o, end, a->created);     o = emit(o, end, "|");
    o = emit_num(o, end, a->last_login);  o = emit(o, end, "|");
    o = emit_num(o, end, a->pw_changed);  o = emit(o, end, "|");
    o = emit_num(o, end, a->fails);       o = emit(o, end, "|");
    o = emit_num(o, end, a->lock_until);  o = emit(o, end, "|");
    o = emit_num(o, end, a->iters);       o = emit(o, end, "|");
    o = emit(o, end, salt);               o = emit(o, end, "|");
    o = emit(o, end, hash);               o = emit(o, end, "|");
    o = emit_num(o, end, a->hist_count);  o = emit(o, end, "|");
    o = emit(o, end, hist);
    if (o >= end) return -1;
    *o++ = '\n'; *o = 0;
    return (int)(o - out);
}

int sc_account_decode(const char *line, struct sc_account *a)
{
    char buf[SC_RECORD_MAX], *f[17];
    int n;
    if (!line || sc_len(line) >= sizeof(buf)) return 0;
    sc_cpy(buf, line);
    n = split_fields(buf, '|', f, 16);
    if (n != 16 || !sc_eq(f[0], "U")) return 0;
    for (int i = 1; i < 16; i++) if (!f[i]) return 0;
    if (!sc_username_valid(f[2])) return 0;
    if (sc_len(f[3]) >= SC_DISPLAY_MAX) return 0;
    for (const char *q = f[3]; *q; q++) if (*q < 32 || *q > 126 || *q == '|') return 0;

    uint32_t u;
    if (!field_u32(f[1], &a->uid) || a->uid < 1000) return 0;
    if (!field_u32(f[4], &a->role) || a->role > SC_ROLE_GUEST) return 0;
    if (!field_u32(f[5], &a->flags) || (a->flags & ~(SC_FLAG_DISABLED|SC_FLAG_MUSTCHANGE))) return 0;
    if (!field_u64(f[6], &a->created)) return 0;
    if (!field_u64(f[7], &a->last_login)) return 0;
    if (!field_u64(f[8], &a->pw_changed)) return 0;
    if (!field_u32(f[9], &a->fails) || a->fails > 1000000) return 0;
    if (!field_u64(f[10], &a->lock_until)) return 0;
    if (!field_u32(f[11], &a->iters) || a->iters < 10000 || a->iters > 2000000) return 0;
    if (sc_len(f[12]) != SC_SALT_BYTES*2 || sc_unhex(a->salt, sizeof(a->salt), f[12], SC_SALT_BYTES*2)) return 0;
    if (sc_len(f[13]) != SC_HASH_BYTES*2 || sc_unhex(a->hash, sizeof(a->hash), f[13], SC_HASH_BYTES*2)) return 0;
    if (!field_u32(f[14], &u) || u > SC_HISTORY_MAX) return 0;
    a->hist_count = (uint8_t)u;
    if (sc_len(f[15]) != (size_t)a->hist_count * 96) return 0;
    if (a->hist_count && sc_unhex(&a->hist[0][0], sizeof(a->hist), f[15], sc_len(f[15]))) return 0;
    sc_copy(a->name, sizeof(a->name), f[2]);
    sc_copy(a->display, sizeof(a->display), f[3]);
    a->used = 1;
    return 1;
}

/* --------------------------------------------------------------- usernames --- */
static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

void sc_username_normal(const char *in, char out[SC_NAME_MAX])
{
    size_t i = 0;
    if (in) for (; i + 1 < SC_NAME_MAX && in[i]; i++) out[i] = lower_of(in[i]);
    out[i] = 0;
}

int sc_username_valid(const char *name)
{
    /* "root" is deliberately NOT reserved: the owner asked for a default
     * account named root with no special privileges.  It is just a username. */
    static const char *reserved[] = {
        "nobody", "system", "security", "kernel", "daemon", "guest",
        "default", "scos", "operator", "superuser", "sys", "bin", 0
    };
    char norm[SC_NAME_MAX];
    size_t n;
    int letters = 0;
    if (!name) return 0;
    n = sc_len(name);
    if (n < 3 || n >= SC_NAME_MAX) return 0;
    sc_username_normal(name, norm);
    if (!is_alnum(norm[0])) return 0;
    if (!is_alnum(norm[n-1])) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = norm[i];
        if (c >= 'a' && c <= 'z') letters = 1;
        else if (!is_alnum(c) && c != '.' && c != '_' && c != '-') return 0;
        if (c == '.' && i + 1 < n && norm[i+1] == '.') return 0;
    }
    if (!letters) return 0;
    for (int i = 0; reserved[i]; i++) if (sc_eq(norm, reserved[i])) return 0;
    return 1;
}

/* --------------------------------------------------------------- passwords --- */
static const char *weak_words[] = {
    "password", "passw0rd", "p@ssw0rd", "123456", "1234567", "12345678",
    "123456789", "qwerty", "qwertyuiop", "asdfgh", "zxcvbn", "letmein",
    "welcome", "iloveyou", "admin", "administrator", "changeme", "sunshine",
    "princess", "football", "baseball", "trustno1", "dragon", "master",
    "login", "starwars", "whatever", "secret", "scos2026", "scos1234",
    "abc123", "abcd1234", "qazwsx", "1q2w3e4r", "monkey", "shadow",
    "superman", "batman", "pokemon", "hello123", "testtest", 0
};

static int has_run(const char *pw, size_t n)
{
    size_t run = 1;
    for (size_t i = 1; i <= n; i++) {
        if (i < n && pw[i] == pw[i-1]) { run++; continue; }
        if (run >= 4) return 1;
        run = 1;
    }
    return 0;
}

static int has_walk(const char *pw, size_t n)
{
    for (size_t i = 2; i < n; i++) {
        int a = (unsigned char)pw[i-2], b = (unsigned char)pw[i-1], c = (unsigned char)pw[i];
        if ((b == a + 1 && c == b + 1) || (b == a - 1 && c == b - 1)) {
            if (i + 1 < n) {
                int d = (unsigned char)pw[i+1];
                if (d == c + 1 || d == c - 1) return 1;   /* four-key walk */
            }
        }
    }
    return 0;
}

static int periodic(const char *pw, size_t n)
{
    if (n < 8) return 0;
    for (size_t p = 1; p <= 4; p++) {
        size_t i;
        for (i = p; i < n; i++) if (pw[i] != pw[i-p]) break;
        if (i == n) return 1;
    }
    return 0;
}

int sc_password_bits(const char *pw)
{
    size_t n = sc_len(pw);
    unsigned lower = 0, upper = 0, digit = 0, symbol = 0, distinct = 0;
    uint32_t pool = 0, ip = 0, base, frac, bits256;
    for (size_t i = 0; i < n; i++) {
        char c = pw[i];
        unsigned seen = 0;
        if (c >= 'a' && c <= 'z') lower = 1;
        else if (c >= 'A' && c <= 'Z') upper = 1;
        else if (c >= '0' && c <= '9') digit = 1;
        else symbol = 1;
        for (size_t j = 0; j < i; j++) if (pw[j] == c) { seen = 1; break; }
        if (!seen) distinct++;
    }
    pool = lower*26u + upper*26u + digit*10u + symbol*32u;
    if (!pool || !n) return 0;
    while ((1u << (ip + 1)) <= pool && ip < 30) ip++;
    base = 1u << ip;
    frac = (uint32_t)(((uint64_t)(pool - base) * 256u) / base);
    bits256 = ip * 256u + frac;
    {
        uint64_t bits = ((uint64_t)n * bits256) / 256u;
        if ((uint64_t)distinct * 2 < n) bits = bits > 8 ? bits - 8 : 0;
        return (int)bits;
    }
}

void sc_policy_hint(const struct sc_policy *p, char *out, size_t cap)
{
    char n1[24], n2[24], n3[24];
    sc_fmt(n1, sizeof(n1), p->min_len);
    sc_fmt(n2, sizeof(n2), p->min_classes);
    sc_fmt(n3, sizeof(n3), p->min_bits);
    sc_copy(out, cap, "New passwords: at least ");
    sc_cat(out, cap, n1); sc_cat(out, cap, " characters, ");
    sc_cat(out, cap, n2); sc_cat(out, cap, " of 4 character classes, ~");
    sc_cat(out, cap, n3); sc_cat(out, cap, " bits minimum.");
}

int sc_password_eval(const char *user, const char *pw,
                     const struct sc_policy *p, char *msg, size_t cap)
{
    char low[SC_PASS_MAX + 1], norm[SC_NAME_MAX];
    size_t n;
    unsigned lower = 0, upper = 0, digit = 0, symbol = 0, classes = 0;
    int penalty = 0, bits;
    if (!pw || !p) { sc_copy(msg, cap, "No password supplied."); return 0; }
    n = sc_len(pw);
    if (n < p->min_len) {
        sc_copy(msg, cap, "Password is too short (minimum ");
        { char t[24]; sc_fmt(t, sizeof(t), p->min_len); sc_cat(msg, cap, t); }
        sc_cat(msg, cap, " characters).");
        return 0;
    }
    if (n > SC_PASS_MAX) { sc_copy(msg, cap, "Password is too long."); return 0; }
    for (size_t i = 0; i < n; i++) {
        char c = pw[i];
        if (c < 32 || c > 126) { sc_copy(msg, cap, "Use printable ASCII characters only."); return 0; }
        if (c >= 'a' && c <= 'z') lower = 1;
        else if (c >= 'A' && c <= 'Z') upper = 1;
        else if (c >= '0' && c <= '9') digit = 1;
        else symbol = 1;                /* punctuation and spaces */
    }
    classes = lower + upper + digit + symbol;
    sc_lower(low, sizeof(low), pw);
    sc_username_normal(user ? user : "", norm);
    if (sc_len(norm) >= 3 && sc_find(low, norm)) {
        sc_copy(msg, cap, "The password must not contain your username."); return 0;
    }
    for (int i = 0; weak_words[i]; i++) {
        size_t wl = sc_len(weak_words[i]);
        if (sc_eq(low, weak_words[i])) {
            sc_copy(msg, cap, "That password is on the common-password blacklist."); return 0;
        }
        if (wl < 5 || !sc_find(low, weak_words[i])) continue;
        /* "password1234" is not a new secret because of the suffix.  Reject a
         * common word that dominates the string; otherwise treat it as a
         * (large) entropy penalty rather than an outright refusal. */
        if (wl * 2 >= n) {
            sc_copy(msg, cap, "That password is based on a common word."); return 0;
        }
        penalty += 18;
    }
    if (classes < p->min_classes) {
        sc_copy(msg, cap, "Mix upper case, lower case, digits and symbols (need ");
        { char t[24]; sc_fmt(t, sizeof(t), p->min_classes); sc_cat(msg, cap, t); }
        sc_cat(msg, cap, " different kinds).");
        return 0;
    }
    if (has_run(pw, n)) { sc_copy(msg, cap, "Avoid four or more identical characters in a row."); return 0; }
    if (has_walk(pw, n)) { sc_copy(msg, cap, "Avoid keyboard sequences such as abcd or 4321."); return 0; }
    if (periodic(pw, n)) { sc_copy(msg, cap, "Avoid repeating a short pattern."); return 0; }
    bits = sc_password_bits(pw) - penalty;
    if (bits < 0) bits = 0;
    if ((uint32_t)bits < p->min_bits) {
        sc_copy(msg, cap, "Password is too predictable (~");
        { char t[24]; sc_fmt(t, sizeof(t), (uint64_t)bits); sc_cat(msg, cap, t); }
        sc_cat(msg, cap, " bits). Use longer, less structured words.");
        return 0;
    }
    {
        char t[24];
        sc_fmt(t, sizeof(t), (uint64_t)bits);
        sc_copy(msg, cap, "Strength: ");
        sc_cat(msg, cap, bits >= 90 ? "excellent" : bits >= 65 ? "strong" :
                         bits >= 45 ? "good" : "fair");
        sc_cat(msg, cap, " (~"); sc_cat(msg, cap, t); sc_cat(msg, cap, " bits).");
    }
    sc_burn(low, sizeof(low));
    return 1;
}
