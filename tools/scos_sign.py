#!/usr/bin/env python3
"""Sign the SCos boot manifest with the RSA-2048 release key.

    python3 tools/scos_sign.py manifest.bin signature.bin
    python3 tools/scos_sign.py --self-test

The private key is keys/boot-signing.json.  Set SCOS_SIGN_KEY=/path/to/key.json
to sign a release with a key that is not in this repository; the public half
must already have been written into boot/uefi/trust_anchor.h with
tools/scos_keys.py --generate.  No OpenSSL dependency: PKCS#1 v1.5 is a single
modular exponentiation.
"""
import hashlib
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import scos_sigformat as fmt  # noqa: E402

SHA256_INFO = bytes.fromhex('3031300d060960864801650304020105000420')
DEFAULT_KEY = ROOT / 'keys' / 'boot-signing.json'


def key_path():
    return Path(os.environ.get('SCOS_SIGN_KEY', DEFAULT_KEY))


def load_key():
    path = key_path()
    if not path.is_file():
        raise SystemExit('signed boot needs %s (generate it with tools/scos_keys.py --generate)' % path)
    k = json.loads(path.read_text())
    if k.get('e') != 65537 or int(k['n'], 16).bit_length() != 2048:
        raise SystemExit('invalid signed-boot key: %s' % path)
    return k


def emsa_pkcs1_v15_sha256(message):
    digest = hashlib.sha256(message).digest()
    ps = b'\xff' * (256 - 3 - len(SHA256_INFO) - len(digest))
    return b'\x00\x01' + ps + b'\x00' + SHA256_INFO + digest


def sign(message):
    k = load_key()
    n, d = int(k['n'], 16), int(k['d'], 16)
    em = emsa_pkcs1_v15_sha256(message)
    if len(em) != 256:
        raise ValueError('bad PKCS#1 block length')
    return pow(int.from_bytes(em, 'big'), d, n).to_bytes(256, 'big')


def sign_manifest(manifest):
    return fmt.sigfile(manifest, sign(manifest))


def self_test():
    """Check the signer against the public half, without the UEFI stub."""
    k = load_key()
    n = int(k['n'], 16)
    files = [('KERNEL.ELF', b'not a real kernel'), ('KERNEL.CRC', b'\0\0\0\0'),
             ('DRVLIST.IDX', b'idx'), ('BUILD.TXT', b'record')]
    m = fmt.manifest_bytes(files)
    blob = sign_manifest(m)
    parsed, sig = fmt.parse_sigfile(blob)
    assert parsed == m and len(sig) == 256
    entries = fmt.parse_manifest(parsed)
    assert [e[0] for e in entries] == [f[0] for f in files]
    em = pow(int.from_bytes(sig, 'big'), 65537, n).to_bytes(256, 'big')
    assert em == emsa_pkcs1_v15_sha256(m), 'signature does not verify with the public key'
    corrupt = bytearray(m)
    corrupt[-1] ^= 1
    assert pow(int.from_bytes(sig, 'big'), 65537, n).to_bytes(256, 'big') != \
        emsa_pkcs1_v15_sha256(bytes(corrupt)), 'tampered manifest still verifies'
    print('PASS: RSA-2048/PKCS#1 v1.5 signer, manifest format and tamper detection')
    return 0


def main(argv):
    if argv == ['--self-test']:
        return self_test()
    if len(argv) != 2:
        raise SystemExit(__doc__)
    manifest = Path(argv[0]).read_bytes()
    Path(argv[1]).write_bytes(sign(manifest))
    print('signed %d manifest bytes -> %s' % (len(manifest), argv[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
