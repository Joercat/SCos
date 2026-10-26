/* SCos encrypted vault: the ATA persistence image is one AEAD message.
 *
 *   [ 0 .. 32736 )  cleartext, authenticated metadata header
 *   [ 32736 .. 32768 )  32-byte encrypt-then-MAC tag
 *   [ 32768 .. 131072 ) AES-256-CTR ciphertext of an SCOSFS2 tree
 *
 * The whole header (key slots, account database, audit log, generation, IV,
 * vault id) is the AEAD's associated data.  That means a bit flip anywhere in
 * the pre-authentication metadata is detected when the password unwraps the
 * volume key - no separate MAC, no second key, no trusted counter.  The header
 * has to be cleartext because it is read before anybody has typed a password;
 * it contains no plaintext secrets, only salts, PBKDF2 verifiers and a volume
 * key wrapped under a password-derived KEK.
 *
 * Threat model: this protects a powered-off, removed or imaged disk.  It does
 * not protect a running kernel (there is no ring-3 isolation), a hardware
 * debugger, or a machine whose attacker can replace the unauthenticated EFI
 * stub itself.  See docs/migration/SECURITY.md.
 */
#include "scos.h"
#include "security.h"

/* Little-endian field helpers: the on-disk format must not depend on the
 * compiler's struct padding or on unaligned pointer dereferences. */
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t hdr[SC_VAULT_HDR_BYTES];
static uint8_t vek[SC_VEK_BYTES];
static int have_key, hdr_valid, image_pending;

#define SLOT_AT(i) (hdr + SC_VAULT_SLOT_OFF + (i) * SC_VAULT_SLOT_BYTES)

int vault_disk_present(void) { return fs_image_available() != 0; }
int vault_unlocked(void) { return have_key && vault_disk_present(); }
int vault_header_valid(void) { return hdr_valid; }
int vault_image_pending(void) { return image_pending; }
void vault_mark_applied(void) { image_pending = 0; }
uint32_t vault_db_len(void) { return hdr_valid ? get32(hdr + 12) : 0; }
uint32_t vault_audit_len(void) { return hdr_valid ? get32(hdr + 64) : 0; }
uint32_t vault_choices_len(void) { return hdr_valid ? get32(hdr + 68) : 0; }

void vault_lock(void)
{
    sc_burn(vek, sizeof(vek));
    have_key = 0;
}

void vault_reset(void)
{
    memset(hdr, 0, sizeof(hdr));
    /* A brand-new vault (first boot, or after the owner switched encryption
     * off and back on) must not look OLDER than the off-image anti-rollback
     * anchor, or the next boot would refuse its own freshly created image. */
    put32(hdr + 20, rollback_high_water());
    sc_burn(vek, sizeof(vek));
    hdr_valid = 0;
    image_pending = 0;
    have_key = 0;
}

void vault_set_db(const char *data, uint32_t len)
{
    if (!data || len > SC_DB_MAX) return;
    memset(hdr + SC_VAULT_DB_OFF, 0, SC_DB_MAX);
    memcpy(hdr + SC_VAULT_DB_OFF, data, len);
    put32(hdr + 12, len);
}

void vault_set_audit(const char *data, uint32_t len)
{
    if (!data || len > SC_AUDIT_BYTES) return;
    memset(hdr + SC_VAULT_AUDIT_OFF, 0, SC_AUDIT_BYTES);
    memcpy(hdr + SC_VAULT_AUDIT_OFF, data, len);
    put32(hdr + 64, len);
}

void vault_set_choices(const char *data, uint32_t len)
{
    if (!data || len > SC_VAULT_CHOICES_MAX) return;
    memset(hdr + SC_VAULT_CHOICES_OFF, 0, SC_VAULT_CHOICES_MAX);
    memcpy(hdr + SC_VAULT_CHOICES_OFF, data, len);
    put32(hdr + 68, len);
}

static int structural(const uint8_t *h)
{
    uint32_t i;
    if (memcmp(h, SC_VAULT_MAGIC, 8)) return 0;
    if (get32(h + 8) != SC_VAULT_VERSION) return 0;
    if (!get32(h + 12) || get32(h + 12) > SC_DB_MAX) return 0;
    if (get32(h + 64) > SC_AUDIT_BYTES) return 0;
    if (get32(h + 68) > SC_VAULT_CHOICES_MAX) return 0;
    if (get32(h + 16) != SC_VAULT_SLOTS) return 0;
    if (get32(h + 56) != SC_VAULT_PAYLOAD) return 0;
    if (!(get32(h + 60) & SC_VAULT_FLAG_SEALED)) return 0;
    for (i = 0; i < SC_VAULT_SLOTS; i++) {
        const uint8_t *s = h + SC_VAULT_SLOT_OFF + i * SC_VAULT_SLOT_BYTES;
        uint32_t f = get32(s + 52);
        if (f & ~(SC_VAULT_SLOT_USED | SC_VAULT_SLOT_RECOVERY)) return 0;
        if ((f & SC_VAULT_SLOT_USED) && !s[0]) return 0;
    }
    return 1;
}

int vault_stage(const uint8_t *image, uint32_t len)
{
    if (!vault_disk_present() || !image || len != SC_VAULT_IMAGE_BYTES) return 0;
    if (memcmp(image, SC_VAULT_MAGIC, 8)) return 0;
    memcpy(hdr, image, sizeof(hdr));
    hdr_valid = 0;
    have_key = 0;
    image_pending = 0;
    if (!structural(hdr)) return 0;
    if (!sc_db_verify((const char *)(hdr + SC_VAULT_DB_OFF), get32(hdr + 12)))
        return 0;
    hdr_valid = 1;
    image_pending = 1;
    return 1;
}

int vault_open(uint8_t *image, uint32_t len)
{
    if (!have_key || !image || len != SC_VAULT_IMAGE_BYTES) return 0;
    if (memcmp(image, SC_VAULT_MAGIC, 8) || !structural(image)) return 0;
    if (!sc_db_verify((const char *)(image + SC_VAULT_DB_OFF), get32(image + 12))) return 0;
    if (!sc_aead_open(vek, image + 40, image, SC_VAULT_TAG_OFF,
                      image + SC_VAULT_HDR_BYTES, SC_VAULT_PAYLOAD,
                      image + SC_VAULT_HDR_BYTES, image + SC_VAULT_TAG_OFF)) {
        klog_raw("security: vault authentication FAILED - disk metadata or data was modified");
        return 0;
    }
    memcpy(hdr, image, sizeof(hdr));
    hdr_valid = 1;
    return 1;
}

uint32_t vault_generation(void) { return hdr_valid ? get32(hdr + 20) : 0; }

int vault_seal(uint8_t *image, uint32_t len)
{
    uint32_t gen;
    if (!have_key || !image || len != SC_VAULT_IMAGE_BYTES) return 0;
    if (!get32(hdr + 12) ||
        !sc_db_verify((const char *)(hdr + SC_VAULT_DB_OFF), get32(hdr + 12))) {
        klog_raw("security: refusing to seal a vault with an invalid account database");
        return 0;
    }
    memcpy(hdr, SC_VAULT_MAGIC, 8);
    put32(hdr + 8, SC_VAULT_VERSION);
    put32(hdr + 16, SC_VAULT_SLOTS);
    put32(hdr + 56, SC_VAULT_PAYLOAD);
    put32(hdr + 60, SC_VAULT_FLAG_SEALED);
    gen = get32(hdr + 20) + 1;
    put32(hdr + 20, gen);
    security_random_bytes(hdr + 40, 16);      /* fresh IV for every save */
    memset(hdr + SC_VAULT_TAG_OFF, 0, 32);
    memcpy(image, hdr, SC_VAULT_TAG_OFF);
    sc_aead_seal(vek, image + 40, image, SC_VAULT_TAG_OFF,
                 image + SC_VAULT_HDR_BYTES, SC_VAULT_PAYLOAD,
                 image + SC_VAULT_HDR_BYTES, image + SC_VAULT_TAG_OFF);
    memcpy(hdr + SC_VAULT_TAG_OFF, image + SC_VAULT_TAG_OFF, 32);
    hdr_valid = 1;
    image_pending = 0;
    return 1;
}

/* ------------------------------------------------------------------ slots --- */
static int slot_of(const char *name, int recovery)
{
    int i, want = recovery ? 1 : 0;
    if (!name) return -1;
    for (i = 0; i < SC_VAULT_SLOTS; i++) {
        const uint8_t *s = SLOT_AT(i);
        uint32_t f = get32(s + 52);
        if (!(f & SC_VAULT_SLOT_USED)) continue;
        if (!!(f & SC_VAULT_SLOT_RECOVERY) != want) continue;
        if (sc_eq((const char *)s, name)) return i;
    }
    return -1;
}

static int free_slot(void)
{
    int i;
    for (i = 0; i < SC_VAULT_SLOTS; i++)
        if (!(get32(SLOT_AT(i) + 52) & SC_VAULT_SLOT_USED)) return i;
    return -1;
}

static void make_slot(int i, const char *name, const char *secret,
                      uint32_t iters, int recovery)
{
    uint8_t *s = SLOT_AT(i);
    uint8_t blocks[64];
    memset(s, 0, SC_VAULT_SLOT_BYTES);
    sc_copy((char *)s, 32, name);
    security_random_bytes(s + 32, SC_SALT_BYTES);
    put32(s + 48, iters ? iters : 100000);
    put32(s + 52, SC_VAULT_SLOT_USED | (recovery ? SC_VAULT_SLOT_RECOVERY : 0));
    sc_pbkdf2_blocks(secret, sc_len(secret), s + 32, SC_SALT_BYTES,
                     get32(s + 48), 2, blocks);
    memcpy(s + 56, blocks, SC_HASH_BYTES);            /* login verifier */
    sc_vek_wrap(blocks + SC_HASH_BYTES, s + 32, vek, s + 88);
    sc_burn(blocks, sizeof(blocks));
    put32(hdr + 16, SC_VAULT_SLOTS);
}

static int create_vault(const char *name, const char *pw, uint32_t iters)
{
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, SC_VAULT_MAGIC, 8);
    put32(hdr + 8, SC_VAULT_VERSION);
    put32(hdr + 16, SC_VAULT_SLOTS);
    put32(hdr + 20, 1);
    put32(hdr + 56, SC_VAULT_PAYLOAD);
    security_random_bytes(vek, sizeof(vek));
    security_random_bytes(hdr + 24, 16);              /* vault id */
    have_key = 1;
    hdr_valid = 1;
    image_pending = 0;                               /* legacy/blank tree is already in RAM */
    make_slot(0, name, pw, iters, 0);
    klog_raw("security: new encrypted vault created; volume key wrapped to the first account");
    return 1;
}

static int derive_slot(const uint8_t *s, const char *secret, uint8_t out[64])
{
    uint32_t iters = get32(s + 48);
    if (iters < 10000 || iters > 2000000) return 0;
    sc_pbkdf2_blocks(secret, sc_len(secret), s + 32, SC_SALT_BYTES, iters, 2, out);
    return 1;
}

static int unlock_slot(int i, const char *secret, char *msg, size_t cap)
{
    const uint8_t *s = SLOT_AT(i);
    uint8_t blocks[64], candidate[SC_VEK_BYTES];
    int ok = 1;
    if (!derive_slot(s, secret, blocks)) { klog_raw("security: vault slot has invalid iteration count"); ok = 0; }
    if (ok && !sc_ct_eq(blocks, s + 56, SC_HASH_BYTES)) {
        klog_raw("security: vault slot verifier mismatch"); ok = 0;
    }
    if (ok && !sc_vek_unwrap(blocks + 32, s + 32, s + 88, candidate)) {
        klog_raw("security: vault slot wrap tag mismatch"); ok = 0;
    }
    if (ok && have_key && !sc_ct_eq(candidate, vek, SC_VEK_BYTES)) {
        klog_raw("security: vault slot holds a different volume key"); ok = 0;
    }
    if (ok && !have_key) { memcpy(vek, candidate, sizeof(vek)); have_key = 1; }
    sc_burn(blocks, sizeof(blocks));
    sc_burn(candidate, sizeof(candidate));
    if (!ok) sc_copy(msg, cap, "Vault key not accepted for this account.");
    return ok;
}

int vault_unlock_password(const char *name, const char *pw, uint32_t iters,
                           char *msg, size_t cap)
{
    int i;
    if (!vault_disk_present()) return 1;              /* RAM-only machines have no vault */
    if (!hdr_valid) return create_vault(name, pw, iters);
    i = slot_of(name, 0);
    if (i < 0) {
        if (!have_key) {
            sc_copy(msg, cap, "This account has no vault key. Ask an administrator "
                              "to set a new password while signed in.");
            return 0;
        }
        i = free_slot();
        if (i < 0) { sc_copy(msg, cap, "Vault key slots are full."); return 0; }
        make_slot(i, name, pw, iters, 0);
        return 1;
    }
    return unlock_slot(i, pw, msg, cap);
}

int vault_unlock_recovery(const char *code, char *msg, size_t cap)
{
    int i;
    if (!vault_disk_present()) return 1;
    i = hdr_valid ? slot_of("@recovery", 1) : -1;
    if (i < 0) { sc_copy(msg, cap, "No recovery key is stored in this vault."); return 0; }
    return unlock_slot(i, code, msg, cap);
}

int vault_rebind_password(const char *name, const char *pw, uint32_t iters)
{
    int i;
    if (!vault_disk_present()) return 1;
    if (!have_key) return 0;
    if (!hdr_valid && !create_vault(name, pw, iters)) return 0;
    i = slot_of(name, 0);
    if (i < 0) i = free_slot();
    if (i < 0) return 0;
    make_slot(i, name, pw, iters, 0);
    return 1;
}

int vault_unbind(const char *name)
{
    int i;
    if (!vault_disk_present()) return 1;
    i = slot_of(name, 0);
    if (i < 0) return 1;
    memset(SLOT_AT(i), 0, SC_VAULT_SLOT_BYTES);
    return 1;
}

int vault_bind_recovery(const char *code, uint32_t iters)
{
    int i;
    if (!vault_disk_present()) return 1;
    if (!have_key) return 0;
    i = slot_of("@recovery", 1);
    if (i < 0) i = free_slot();
    if (i < 0) return 0;
    make_slot(i, "@recovery", code, iters, 1);
    return 1;
}

void vault_forget_recovery(void)
{
    int i = slot_of("@recovery", 1);
    if (i >= 0) memset(SLOT_AT(i), 0, SC_VAULT_SLOT_BYTES);
}
