/* Host harness for the exact RSA verifier object linked into BOOTX64.EFI.
 * Usage: rsa_host <message file> <256-byte signature> <256-byte modulus>
 * Prints 1 when the signature verifies, 0 otherwise.  Compiled against
 * boot/uefi/rsa_verify.c and kernel/desktop/sec_core.c on the host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int boot_rsa_verify(const uint8_t *msg, size_t len,
                    const uint8_t sig[256], const uint8_t pub_n[256]);

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f); return 0;
    }
    p = (uint8_t *)malloc((size_t)n + 1);
    if (!p) { fclose(f); return 0; }
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return 0; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(int argc, char **argv)
{
    size_t mlen = 0, slen = 0, nlen = 0;
    uint8_t *msg, *sig, *n;
    int ok;
    if (argc != 4) return 2;
    msg = slurp(argv[1], &mlen);
    sig = slurp(argv[2], &slen);
    n = slurp(argv[3], &nlen);
    if (!msg || !sig || !n || slen != 256 || nlen != 256) return 2;
    ok = boot_rsa_verify(msg, mlen, sig, n);
    printf("%d\n", ok);
    free(msg); free(sig); free(n);
    return 0;
}
