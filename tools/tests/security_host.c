/* Host-side known-answer tests for the SCos security core.
 * Compiled by tools/tests/test_security.py against kernel/desktop/sec_core.c.
 * Every PASS line names a distinct property; test_security.py asserts the
 * count so the suite cannot silently shrink.
 */
#include "security.h"
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int ok, const char *name)
{
    checks++;
    if (!ok) failures++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}

static void hex_of(const uint8_t *in, size_t n, char *out)
{
    sc_hex(out, n * 2 + 1, in, n);
}

static void sha(const char *text, size_t len, char *out)
{
    uint8_t d[SC_HASH_BYTES];
    sc_sha256_hash(text, len, d);
    hex_of(d, SC_HASH_BYTES, out);
}

static void test_sha256(void)
{
    char got[65];
    char million[1024];
    sha("", 0, got);
    check(!strcmp(got, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256 empty string (FIPS 180-4)");
    sha("abc", 3, got);
    check(!strcmp(got, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 'abc' (FIPS 180-4)");
    sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, got);
    check(!strcmp(got, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "SHA-256 two-block message (FIPS 180-4)");
    /* 448 bytes in one update, then 552: forces the 56-byte padding boundary. */
    memset(million, 'a', sizeof(million));
    {
        sc_sha256 c;
        uint8_t d[SC_HASH_BYTES];
        sc_sha256_init(&c);
        for (int i = 0; i < 1000; i++) sc_sha256_update(&c, million, 1000);
        sc_sha256_final(&c, d);
        hex_of(d, SC_HASH_BYTES, got);
    }
    check(!strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "SHA-256 one million 'a' (streaming update)");
}

static void test_hmac(void)
{
    uint8_t key[20], d[SC_HASH_BYTES];
    char got[65];
    memset(key, 0x0b, sizeof(key));
    sc_hmac_hash(key, sizeof(key), "Hi There", 8, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "HMAC-SHA256 RFC 4231 case 1");
    sc_hmac_hash("Jefe", 4, "what do ya want for nothing?", 28, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
          "HMAC-SHA256 RFC 4231 case 2");
    /* A key longer than one block takes the hash-the-key branch. */
    {
        char longkey[200];
        memset(longkey, 'k', sizeof(longkey));
        sc_hmac_hash(longkey, sizeof(longkey), "msg", 3, d);
        hex_of(d, SC_HASH_BYTES, got);
        check(!strcmp(got, "e2adadca233bc31c6e6126c865132c3e945f9dedd44797a1e5acc3c037bc21fc"),
              "HMAC-SHA256 hashes a key longer than 64 bytes");
    }
}

static void test_pbkdf2(void)
{
    uint8_t d[SC_HASH_BYTES];
    char got[65];
    sc_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 1, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
          "PBKDF2-HMAC-SHA256 password/salt/1 (RFC 6070 family)");
    sc_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 2, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"),
          "PBKDF2-HMAC-SHA256 password/salt/2");
    sc_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 4096, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
          "PBKDF2-HMAC-SHA256 password/salt/4096");
    sc_pbkdf2("passwordPASSWORDpassword", 24,
              (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, d);
    hex_of(d, SC_HASH_BYTES, got);
    check(!strcmp(got, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1"),
          "PBKDF2-HMAC-SHA256 long password and salt/4096");
    /* A password longer than the HMAC block size must take the hash-key path. */
    {
        char pw[130];
        memset(pw, 'x', sizeof(pw));
        pw[sizeof(pw)-1] = 0;
        sc_pbkdf2(pw, sizeof(pw)-1, (const uint8_t *)"s", 1, 3, d);
        hex_of(d, SC_HASH_BYTES, got);
        check(got[0] != 0 && got[64] == 0, "PBKDF2 accepts a 129-byte password");
    }
}

static void test_helpers(void)
{
    uint8_t a[32], b[32], raw[16];
    char hex[65];
    uint64_t v;
    memset(a, 0x5a, sizeof(a));
    memcpy(b, a, sizeof(b));
    check(sc_ct_eq(a, b, sizeof(a)), "constant-time compare accepts equal buffers");
    b[31] ^= 1;
    check(!sc_ct_eq(a, b, sizeof(a)), "constant-time compare rejects a one-bit difference");
    sc_hex(hex, sizeof(hex), a, sizeof(a));
    check(strlen(hex) == 64, "hex encoder length");
    check(!sc_unhex(raw, sizeof(raw), hex, 32) && raw[0] == 0x5a && raw[15] == 0x5a,
          "hex decoder round trip");
    check(sc_unhex(raw, sizeof(raw), "zzzz", 4) != 0, "hex decoder rejects non-hex");
    check(sc_num("4096", &v) && v == 4096, "strict decimal parser");
    check(!sc_num("12x", &v) && !sc_num("", &v), "strict decimal parser rejects junk");
    sc_burn(a, sizeof(a));
    check(a[0] == 0 && a[31] == 0, "secret scrubbing zeroes the buffer");
}

static void test_usernames(void)
{
    char norm[SC_NAME_MAX];
    check(sc_username_valid("admin"), "plain username accepted");
    check(sc_username_valid("Ada.Lovelace-2"), "mixed case, dot and dash accepted");
    check(!sc_username_valid("ab"), "two-character username rejected");
    check(!sc_username_valid("has space"), "whitespace rejected");
    check(!sc_username_valid("../etc"), "path traversal rejected");
    check(sc_username_valid("root"), "'root' is a normal username, not reserved");
    check(!sc_username_valid("System"), "reserved name is case-insensitive");
    check(!sc_username_valid("123"), "purely numeric name rejected");
    check(!sc_username_valid("bad..name"), "double dot rejected");
    sc_username_normal("Ada.Lovelace", norm);
    check(!strcmp(norm, "ada.lovelace"), "username normalization lowercases");
}

static void test_passwords(void)
{
    struct sc_policy p;
    char msg[SC_MSG_MAX];
    sc_policy_default(&p);
    check(!sc_password_eval("alice", "password", &p, msg, sizeof(msg)) &&
          sc_find(msg, "short"), "short password rejected");
    check(!sc_password_eval("alice", "password1234", &p, msg, sizeof(msg)) &&
          sc_find(msg, "common word"), "common word plus digits rejected");
    check(!sc_password_eval("alice", "Password1234!", &p, msg, sizeof(msg)) &&
          sc_find(msg, "common word"), "common-word rejection is case-insensitive");
    check(!sc_password_eval("alice", "administrator", &p, msg, sizeof(msg)) &&
          sc_find(msg, "blacklist"), "exact blacklist match rejected");
    check(!sc_password_eval("alice", "alllowercase", &p, msg, sizeof(msg)) &&
          sc_find(msg, "Mix"), "single character class rejected");
    check(!sc_password_eval("alice", "Alicealice2026!", &p, msg, sizeof(msg)) &&
          sc_find(msg, "username"), "password containing username rejected");
    check(!sc_password_eval("alice", "Klmn9876#Qw!", &p, msg, sizeof(msg)) &&
          sc_find(msg, "keyboard"), "keyboard walk rejected");
    check(!sc_password_eval("alice", "Aaaa1111!xyz", &p, msg, sizeof(msg)) &&
          sc_find(msg, "identical"), "long repeated run rejected");
    check(!sc_password_eval("alice", "Ab1!Ab1!Ab1!", &p, msg, sizeof(msg)) &&
          sc_find(msg, "short pattern"), "periodic password rejected");
    check(sc_password_eval("alice", "Xk9#mQ2!vLp7", &p, msg, sizeof(msg)) &&
          sc_find(msg, "Strength"), "a 12-character mixed password is accepted");
    check(sc_password_eval("alice", "Xk9#mQ2!vLp7Zt4$", &p, msg, sizeof(msg)) &&
          sc_find(msg, "excellent"), "a longer password scores higher");
    {
        char hint[160];
        sc_policy_hint(&p, hint, sizeof(hint));
        check(sc_find(hint, "12 characters") && sc_find(hint, "3 of 4"),
              "policy hint states the live minimums");
    }
}

static void test_codec(void)
{
    struct sc_policy p;
    struct sc_account a, b;
    char line[SC_RECORD_MAX], pline[256], body[SC_DB_MAX], db[SC_DB_MAX], digest[65];
    size_t n;
    sc_policy_default(&p);
    check(sc_policy_encode(&p, pline, sizeof(pline)) > 0, "policy line encodes");
    {
        struct sc_policy q;
        char trimmed[256];
        size_t l = strlen(pline);
        memcpy(trimmed, pline, l + 1);
        if (l && trimmed[l-1] == '\n') trimmed[l-1] = 0;
        memset(&q, 0, sizeof(q));
        check(sc_policy_decode(trimmed, &q) && q.min_len == p.min_len &&
              q.iters == p.iters && q.lock_max == p.lock_max, "policy line round trips");
        trimmed[5] = trimmed[5] == '9' ? '8' : '9';
        check(!sc_policy_decode(trimmed, &q), "tampered policy line is refused");
    }
    memset(&a, 0, sizeof(a));
    sc_cpy(a.name, "ada.lovelace");
    sc_cpy(a.display, "ada.lovelace");
    a.uid = 1001; a.role = SC_ROLE_USER; a.flags = SC_FLAG_MUSTCHANGE;
    a.created = 1700000000; a.last_login = 1700000001; a.pw_changed = 1700000002;
    a.lock_until = 1700000003; a.fails = 2; a.iters = 100000;
    for (int i = 0; i < SC_SALT_BYTES; i++) a.salt[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < SC_HASH_BYTES; i++) a.hash[i] = (uint8_t)(i * 3 + 5);
    a.hist_count = 1;
    for (int i = 0; i < SC_SALT_BYTES + SC_HASH_BYTES; i++) a.hist[0][i] = (uint8_t)(i ^ 0xa5);
    a.used = 1;
    check(sc_account_encode(&a, line, sizeof(line)) > 0, "account record encodes");
    memset(&b, 0, sizeof(b));
    {
        char trimmed[SC_RECORD_MAX];
        size_t l = strlen(line);
        memcpy(trimmed, line, l + 1);
        if (l && trimmed[l-1] == '\n') trimmed[l-1] = 0;
        check(sc_account_decode(trimmed, &b), "account record decodes");
        check(b.uid == a.uid && b.role == a.role && b.flags == a.flags &&
              b.created == a.created && b.last_login == a.last_login &&
              b.pw_changed == a.pw_changed && b.lock_until == a.lock_until &&
              b.fails == a.fails && b.iters == a.iters, "numeric fields round trip");
        check(!strcmp(a.name, b.name) && b.hist_count == 1 &&
              !memcmp(a.salt, b.salt, sizeof(a.salt)) &&
              !memcmp(a.hash, b.hash, sizeof(a.hash)) &&
              !memcmp(a.hist, b.hist, 48), "salt, hash and history round trip");
        trimmed[40] = 'Z';
        check(!sc_account_decode(trimmed, &b), "tampered account record is refused");
    }
    /* A brand-new account has no history, so the record ends in an empty
     * field.  This is the shape that is written on first run and read back
     * after a reboot with a verified ATA save target. */
    {
        struct sc_account fresh, back;
        char fline[SC_RECORD_MAX], trimmed[SC_RECORD_MAX];
        size_t l;
        memset(&fresh, 0, sizeof(fresh));
        sc_cpy(fresh.name, "newuser");
        sc_cpy(fresh.display, "newuser");
        fresh.uid = 1002; fresh.role = SC_ROLE_USER; fresh.iters = 100000;
        fresh.created = fresh.pw_changed = 1700000100;
        for (int i = 0; i < SC_SALT_BYTES; i++) fresh.salt[i] = (uint8_t)i;
        for (int i = 0; i < SC_HASH_BYTES; i++) fresh.hash[i] = (uint8_t)(255 - i);
        fresh.hist_count = 0; fresh.used = 1;
        check(sc_account_encode(&fresh, fline, sizeof(fline)) > 0,
              "zero-history record encodes (first-run shape)");
        l = strlen(fline);
        memcpy(trimmed, fline, l + 1);
        if (l && trimmed[l-1] == '\n') trimmed[l-1] = 0;
        memset(&back, 0, sizeof(back));
        check(sc_account_decode(trimmed, &back) && back.uid == 1002 &&
              back.hist_count == 0 && !strcmp(back.name, "newuser"),
              "zero-history record round trips after reboot");
    }
    /* Database integrity line: covers exactly the bytes before C|. */
    sc_copy(body, sizeof(body), "SCos-accounts-v1\n");
    sc_cat(body, sizeof(body), pline);
    sc_cat(body, sizeof(body), "R|0|0|0\n");
    sc_cat(body, sizeof(body), line);
    n = strlen(body);
    sc_db_digest(body, n, digest);
    memcpy(db, body, n);
    sc_copy(db + n, sizeof(db) - n, "C|");
    sc_cat(db, sizeof(db), digest);
    sc_cat(db, sizeof(db), "\n");
    check(sc_db_verify(db, strlen(db)), "database digest verifies a genuine file");
    db[30] = db[30] == 'a' ? 'b' : 'a';
    check(!sc_db_verify(db, strlen(db)), "database digest detects a changed byte");
    check(!sc_db_verify(db, strlen(db) - 9), "truncated database is refused");
}

static int unhex_bytes(const char *hex, uint8_t *out, int n)
{
    return sc_unhex(out, (size_t)n, hex, (size_t)n * 2) == 0;
}

static void test_pbkdf2_blocks(void)
{
    uint8_t two[64], one[32];
    char got[129];
    /* RFC 6070-style first block, then a second independent block.  The
     * multi-block API must not change the 32-byte legacy result. */
    sc_pbkdf2_blocks("password", 8, (const uint8_t *)"salt", 4, 4096, 2, two);
    hex_of(two, 64, got);
    check(!strcmp(got,
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"
        "f7ad98c1b458ce3fd74ca35beba3cda7b8d1038d6a87071b918f837405f3fe77"),
        "PBKDF2 two blocks match hashlib.pbkdf2_hmac(dklen=64)");
    sc_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 4096, one);
    check(!memcmp(one, two, 32), "PBKDF2 block 1 equals sc_pbkdf2");
    check(memcmp(two, two + 32, 32), "PBKDF2 output blocks are independent");
    sc_pbkdf2_blocks("SCos-Test", 9, (const uint8_t *)"0123456789abcdef", 16,
                     100000, 2, two);
    hex_of(two, 64, got);
    check(!strcmp(got,
        "a4e45d7ccb71c6e8851f927aa9fb72880b1f0bafd866f6ed6ecfd1138f6920f6"
        "f68ebe67b9705a9eeed83bbf2112a47a3639bd4f9d17a9ef84aa4f87db9b5ecc"),
        "PBKDF2 two blocks with a 16-byte salt and 100000 rounds");
    memset(two, 0xa5, sizeof(two));
    sc_pbkdf2_blocks("p", 1, (const uint8_t *)"s", 1, 1, 0, two);
    check(two[0] == 0xa5, "PBKDF2 zero blocks leaves the buffer untouched");
}

static void test_aes256(void)
{
    uint8_t key[32], pt[64], ct[64], back[64], iv[16];
    char got[129];
    /* FIPS 197 Appendix C.3, AES-256 single block. */
    unhex_bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    unhex_bytes("00112233445566778899aabbccddeeff", pt, 16);
    sc_aes256_encrypt_block(key, pt, ct);
    hex_of(ct, 16, got);
    check(!strcmp(got, "8ea2b7ca516745bfeafc49904b496089"),
          "AES-256 FIPS 197 C.3 known-answer block");

    /* NIST SP 800-38A F.5.5 CTR-AES256.Encrypt, four blocks. */
    unhex_bytes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key, 32);
    unhex_bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    unhex_bytes("6bc1bee22e409f96e93d7e117393172a"
                "ae2d8a571e03ac9c9eb76fac45af8e51"
                "30c81c46a35ce411e5fbc1191a0a52ef"
                "f69f2445df4f9b17ad2b417be66c3710", pt, 64);
    sc_aes256_ctr(key, iv, ct, pt, 64);
    hex_of(ct, 64, got);
    check(!strcmp(got,
        "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
        "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"),
        "AES-256-CTR SP 800-38A F.5.5 four-block vector");
    sc_aes256_ctr(key, iv, back, ct, 64);
    check(!memcmp(back, pt, 64), "AES-256-CTR is its own inverse");
    memset(back, 0xee, sizeof(back));
    sc_aes256_ctr(key, iv, back, ct, 17);
    check(!memcmp(back, pt, 17) && back[17] == 0xee,
          "AES-256-CTR handles a partial final block without overrun");
    {
        uint8_t ks[32], zero[32];
        memset(zero, 0, sizeof(zero));
        sc_aes256_ctr(key, iv, ks, 0, 32);
        sc_aes256_ctr(key, iv, back, zero, 32);
        check(!memcmp(ks, back, 32), "AES-256-CTR NULL input generates keystream");
    }
}

static void test_aead(void)
{
    uint8_t key[32], iv[16], pt[100], ct[100], back[100], tag[32], bad[32];
    const uint8_t ad[24] = "SCos-vault-header-v1-01";
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 5 + 3);
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(255 - i * 3);
    for (int i = 0; i < 100; i++) pt[i] = (uint8_t)(i * 7 + 11);
    sc_aead_seal(key, iv, ad, sizeof(ad), pt, sizeof(pt), ct, tag);
    check(memcmp(ct, pt, sizeof(pt)), "AEAD seal produces ciphertext");
    memset(back, 0xee, sizeof(back));
    check(sc_aead_open(key, iv, ad, sizeof(ad), ct, sizeof(ct), back, tag) &&
          !memcmp(back, pt, sizeof(pt)), "AEAD open round trips 100 bytes");
    ct[0] ^= 1;
    memset(back, 0xee, sizeof(back));
    check(!sc_aead_open(key, iv, ad, sizeof(ad), ct, sizeof(ct), back, tag),
          "AEAD rejects a flipped ciphertext bit");
    check(back[0] == 0 && back[99] == 0,
          "AEAD does not release plaintext when the tag fails");
    ct[0] ^= 1;
    memcpy(bad, tag, 32);
    bad[31] ^= 0x80;
    check(!sc_aead_open(key, iv, ad, sizeof(ad), ct, sizeof(ct), back, bad),
          "AEAD rejects a modified tag");
    {
        uint8_t ad2[24];
        memcpy(ad2, ad, sizeof(ad2));
        ad2[23] ^= 1;
        check(!sc_aead_open(key, iv, ad2, sizeof(ad2), ct, sizeof(ct), back, tag),
              "AEAD binds the associated header data");
    }
    {
        uint8_t k2[32], iv2[16];
        memcpy(k2, key, 32); k2[0] ^= 1;
        memcpy(iv2, iv, 16); iv2[0] ^= 1;
        check(!sc_aead_open(k2, iv, ad, sizeof(ad), ct, sizeof(ct), back, tag),
              "AEAD rejects the wrong key");
        check(!sc_aead_open(key, iv2, ad, sizeof(ad), ct, sizeof(ct), back, tag),
              "AEAD tag covers the IV indirectly through ciphertext");
    }
    check(!sc_aead_open(key, iv, ad, sizeof(ad), ct, 99, back, tag),
          "AEAD rejects a truncated ciphertext");
    {
        uint8_t iv2[16], ct2[100], tag2[32];
        memcpy(iv2, iv, 16); iv2[15] ^= 1;
        sc_aead_seal(key, iv2, ad, sizeof(ad), pt, sizeof(pt), ct2, tag2);
        check(memcmp(ct, ct2, sizeof(ct)) && memcmp(tag, tag2, 32),
              "a fresh IV changes both ciphertext and tag");
    }
    {
        uint8_t short_ct[16], short_tag[32], short_pt[16], short_back[16];
        memset(short_pt, 0x42, sizeof(short_pt));
        sc_aead_seal(key, iv, 0, 0, short_pt, 16, short_ct, short_tag);
        check(sc_aead_open(key, iv, 0, 0, short_ct, 16, short_back, short_tag) &&
              !memcmp(short_pt, short_back, 16), "AEAD handles empty associated data");
    }
    {
        uint8_t empty_tag[32], other[32];
        sc_aead_seal(key, iv, ad, sizeof(ad), 0, 0, 0, empty_tag);
        sc_aead_seal(key, iv, ad, sizeof(ad) - 1, 0, 0, 0, other);
        check(memcmp(empty_tag, other, 32), "AEAD tag depends on AD length");
        check(sc_aead_open(key, iv, ad, sizeof(ad), 0, 0, 0, empty_tag),
              "AEAD accepts a zero-length message with the right tag");
    }
}

static void test_vek_wrap(void)
{
    uint8_t kek[32], nonce[16], vek[32], wrapped[48], back[32];
    for (int i = 0; i < 32; i++) kek[i] = (uint8_t)(i ^ 0x5a);
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(i * 13 + 1);
    for (int i = 0; i < 32; i++) vek[i] = (uint8_t)(i * 9 + 7);
    sc_vek_wrap(kek, nonce, vek, wrapped);
    check(memcmp(wrapped, vek, 32), "VEK wrap hides the volume key");
    check(sc_vek_unwrap(kek, nonce, wrapped, back) && !memcmp(back, vek, 32),
          "VEK wrap/unwrap round trip");
    {
        uint8_t zero_wrap[48], k2[32], w2[48], n2[16];
        memset(zero_wrap, 0, sizeof(zero_wrap));
        check(!sc_vek_unwrap(kek, nonce, zero_wrap, back),
              "an all-zero wrapped volume key is rejected");
        memcpy(k2, kek, 32); k2[31] ^= 1;
        memset(back, 0xcc, sizeof(back));
        check(!sc_vek_unwrap(k2, nonce, wrapped, back) && back[0] == 0,
              "wrong KEK cannot unwrap the volume key");
        memcpy(w2, wrapped, 48); w2[40] ^= 1;
        check(!sc_vek_unwrap(kek, nonce, w2, back), "tampered VEK tag is rejected");
        memcpy(w2, wrapped, 48); w2[3] ^= 1;
        check(!sc_vek_unwrap(kek, nonce, w2, back), "tampered VEK ciphertext is rejected");
        memcpy(n2, nonce, 16); n2[15] ^= 1;
        check(!sc_vek_unwrap(kek, n2, wrapped, back), "VEK wrap binds the KDF salt as nonce");
        sc_vek_wrap(kek, nonce, vek, w2);
        check(!memcmp(w2, wrapped, 48), "VEK wrap is deterministic for a fixed KEK and salt");
    }
}

int main(void)
{
    test_sha256();
    test_hmac();
    test_pbkdf2();
    test_pbkdf2_blocks();
    test_aes256();
    test_aead();
    test_vek_wrap();
    test_helpers();
    test_usernames();
    test_passwords();
    test_codec();
    printf("TOTAL: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
