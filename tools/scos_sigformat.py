"""Signed-boot manifest wire format, shared by the packer, the signer and tests.

Kept deliberately tiny and fixed-size so the freestanding UEFI stub can parse
it with no allocator and no variable-length surprises:

BOOT.SIG
    0   8   "SCOSSIG1"
    8   4   version = 1 (LE u32)
    12  4   manifest_len (LE u32)
    16  256 RSA-2048 signature over SHA-256(manifest)
    272 ... manifest (manifest_len bytes)

manifest
    0   8   "SCOSMAN1"
    8   4   version = 1
    12  4   entry_count
    16  4   reserved = 0
    20  32  domain string
    52  ... entries, 48 bytes each: name[12] (8.3 with a dot, NUL padded),
                                  size u32 LE, sha256[32]

The manifest lists every executable or security-relevant file in \\SCOS.  A
file that is not listed can still be present; a file that IS listed must exist
at the listed size and hash.  The stub requires the four mandatory names, so
removing the manifest entry for KERNEL.ELF does not help an attacker.
"""
import hashlib
import struct

SIG_MAGIC = b'SCOSSIG1'
MAN_MAGIC = b'SCOSMAN1'
VERSION = 1
SIG_HEADER = 272
SIG_BYTES = 256
MAN_HEADER = 52
ENTRY_BYTES = 48
MAN_DOMAIN = b'SCos x64 UEFI signed boot v1'  # 29 bytes
MANDATORY = ('KERNEL.ELF', 'KERNEL.CRC', 'DRVLIST.IDX', 'BUILD.TXT')
MAX_ENTRIES = 24


def valid_name(name):
    """Only uppercase 8.3 ASCII, one dot, no path characters. Stub uses this."""
    if not isinstance(name, str) or not 1 <= len(name) <= 12:
        return False
    if name.count('.') != 1:
        return False
    stem, ext = name.split('.')
    if not 1 <= len(stem) <= 8 or not 1 <= len(ext) <= 3:
        return False
    return all(c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in name)


def manifest_bytes(files):
    """files: ordered iterable of (name, blob). Deterministic for fixed input."""
    entries = []
    for name, blob in files:
        if not valid_name(name):
            raise ValueError('not a valid 8.3 manifest name: %r' % name)
        if len(blob) > 0xffffffff:
            raise ValueError('manifest entry too large')
        entries.append(name.encode('ascii').ljust(12, b'\0') +
                       struct.pack('<I', len(blob)) + hashlib.sha256(blob).digest())
    if len(entries) > MAX_ENTRIES:
        raise ValueError('too many manifest entries')
    names = [e[:12].split(b'\0')[0].decode() for e in entries]
    for required in MANDATORY:
        if required not in names:
            raise ValueError('mandatory boot file missing from manifest: %s' % required)
    if len(set(names)) != len(names):
        raise ValueError('duplicate manifest entry')
    out = (MAN_MAGIC + struct.pack('<III', VERSION, len(entries), 0) +
           MAN_DOMAIN.ljust(32, b'\0'))
    return out + b''.join(entries)


def sigfile(manifest, signature):
    if len(signature) != SIG_BYTES:
        raise ValueError('RSA-2048 signature must be exactly 256 bytes')
    return (SIG_MAGIC + struct.pack('<II', VERSION, len(manifest)) +
            signature + manifest)


def parse_sigfile(blob):
    """Validate the outer container; returns (manifest, signature)."""
    if len(blob) < SIG_HEADER + MAN_HEADER or blob[:8] != SIG_MAGIC:
        raise ValueError('not an SCos signed-boot file')
    version, mlen = struct.unpack_from('<II', blob, 8)
    if version != VERSION or len(blob) != SIG_HEADER + mlen:
        raise ValueError('signed-boot container length/version mismatch')
    return blob[SIG_HEADER:], blob[16:SIG_HEADER]


def parse_manifest(manifest):
    """Returns a list of (name, size, sha256). Raises on any malformed field."""
    if len(manifest) < MAN_HEADER or manifest[:8] != MAN_MAGIC:
        raise ValueError('bad manifest magic')
    version, count, reserved = struct.unpack_from('<III', manifest, 8)
    if version != VERSION or reserved or not 1 <= count <= MAX_ENTRIES:
        raise ValueError('bad manifest header')
    if manifest[20:20 + len(MAN_DOMAIN)] != MAN_DOMAIN:
        raise ValueError('manifest is from a different signing domain')
    if len(manifest) != MAN_HEADER + count * ENTRY_BYTES:
        raise ValueError('manifest entry count does not match its length')
    out = []
    for i in range(count):
        raw = manifest[MAN_HEADER + i * ENTRY_BYTES:][:ENTRY_BYTES]
        field = raw[:12]
        if b'\0' not in field:
            raise ValueError('unterminated manifest file name')
        name = field.split(b'\0')[0].decode('ascii')
        if not valid_name(name):
            raise ValueError('invalid manifest file name')
        size, = struct.unpack_from('<I', raw, 12)
        out.append((name, size, raw[16:48]))
    names = [e[0] for e in out]
    for required in MANDATORY:
        if required not in names:
            raise ValueError('mandatory boot file missing: %s' % required)
    return out
