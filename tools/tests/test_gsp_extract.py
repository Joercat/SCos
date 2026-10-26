#!/usr/bin/env python3
"""The firmware extractor: the driver's own arrays, rebuilt into the container this kernel boots from.

`tools/gsp-firmware.py` reads the generated `g_bindata_kgspGetBinArchive*.c` files the open kernel module ships
- the FMC image's four pieces, as the vendor packs them - and wraps them in the ELF32 container whose shape the
kernel's parser and nouveau's `gh100_gsp_oneinit()` both demand.  Run against the real arrays for GB202 at
615.71.10 it produced `hash 48, publickey 97, signature 96, image 287304` and then reported that the 48-byte
hash *is* SHA-384 of the image, byte for byte - the first time this OS's per-generation widths were confirmed
against NVIDIA's own bytes rather than against a driver's source.  That measurement is what these fixtures
reproduce, because the vendor's arrays cannot be fetched from here and a test must not depend on a download:
each case is one rule of the format, with a container the checker then has to accept.
"""

import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = str(ROOT / 'tools' / 'gsp-firmware.py')
CHECKER = str(ROOT / 'tools' / 'gspcheck.py')
ARCHIVE = 'g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_%s.c'


def array(blob):
    """The bytes as the generated file writes them: little-endian NvU64 words, zero-padded to a whole word,
    which is why a 97-byte public key occupies 104."""
    pad = (-len(blob)) % 8
    words = blob + bytes(pad)
    return '\n'.join('    ' + ', '.join('0x%016xULL' % struct.unpack_from('<Q', words, i)[0]
                                        for i in range(row * 2 * 8, min(row * 2 * 8 + 16, len(words)), 8)) + ','
                     for row in range((len(words) + 15) // 16)) + '\n'


def entry(label, blob, note, declared=None, compressed=False):
    """One `// FUNCTION: ... LABEL ... / DATA SIZE / COMPRESSED SIZE` comment block plus its array, exactly as
    the generated file writes it.  `declared' is the uncompressed length, which for a compressed entry is not
    the length of the bytes in the array - that difference is the whole reason the tool needs both."""
    declared = len(blob) if declared is None else declared
    header = ('//\n// FUNCTION: kgspGetBinArchiveGspRmFmcGfwProdSigned_%s("BINDATA_LABEL_%s")\n'
              '// FILE NAME: kernel/inc/gspcc/bin/%s\n// FILE TYPE: BINARY\n// VAR NAME: N/A\n'
              '// COMPRESSION: %s\n// COMPLEX_STRUCT: NO\n// DATA SIZE (bytes): %d\n'
              '// COMPRESSED SIZE (bytes): %d\n// EXTERNAL: NO\n//\n'
              % (note, label, 'g_%s.bin' % label.lower(), 'YES' if compressed else 'NO', declared,
                 declared if not compressed else len(blob)))
    var = 'kgspBinArchiveGspRmFmcGfwProdSigned_%s_BINDATA_LABEL_%s_data' % (note, label)
    return (header + 'static BINDATA_CONST NV_DECLARE_ALIGNED(NvU64, 8) %s[] = \n{\n%s};\n'
            % (var, array(blob)))


def deflate(blob):
    compress = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compress.compress(blob) + compress.flush()


def archive(chip='GB202', image=None, hash_bytes=None, pkey=97, sig=96, compress_image=False, truncate=None):
    image = image if image is not None else bytes((i * 7 + 3) & 0xff for i in range(0x400))
    digest = hash_bytes if hash_bytes is not None else hashlib.sha384(image).digest()
    payload, declared, compressed = image, len(image), False
    if compress_image:
        payload, declared, compressed = deflate(image), len(image), True
    if truncate is not None:
        payload = payload[:truncate]
    pieces = [('UCODE_HASH', digest, None, False), ('UCODE_PKEY', bytes(range(1, pkey + 1)), None, False),
              ('UCODE_SIG', bytes((i * 5) & 0xff for i in range(sig)), None, False),
              ('UCODE_IMAGE', payload, declared, compressed)]
    body = ''.join(entry(label, blob, chip, declared=d, compressed=c) for label, blob, d, c in pieces)
    return ('/* generated */\n#include "bindata.h"\n\n#if defined(BINDATA_INCLUDE_DATA)\n' + body +
            '#endif // defined(BINDATA_INCLUDE_DATA)\n'), image, digest


def run(*args):
    return subprocess.run([sys.executable] + list(args), cwd=ROOT, capture_output=True, text=True)


def extract(src, out, chip='GB202', version='1.2.3'):
    result = run(TOOL, '--dir', src, '--version', version, '--chip', chip, '--out', out)
    return result


def test_the_four_pieces_become_the_container_this_kernel_reads():
    with tempfile.TemporaryDirectory() as tmp:
        src, out = os.path.join(tmp, 'src'), os.path.join(tmp, 'out')
        os.makedirs(src)
        text, image, digest = archive()
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text)
        result = extract(src, out)
        assert result.returncode == 0, result.stdout + result.stderr
        # The claim the real arrays produced, reproduced here on purpose: the hash is the image's SHA-384.
        assert 'the hash section is sha384 of the image section' in result.stdout, result.stdout
        flat = re.sub(r'\s+', ' ', result.stdout)
        assert 'hash %d B' % len(digest) in flat, result.stdout
        container = os.path.join(out, 'fmc-1.2.3.bin')
        blob = open(container, 'rb').read()
        # The header the kernel compares byte for byte, and the six-section layout it refuses anything else as.
        sys.path.insert(0, str(ROOT / 'tools'))
        import gspcheck
        assert blob[:52] == gspcheck.FMC_HEADER, 'the container does not begin with the FMC header itself'
        assert struct.unpack_from('<H', blob, 48)[0] == 6
        # And the image is carried unmodified, so the panel's number is about the vendor's bytes.
        shoff = 52
        image_row = shoff + 5 * 40
        off, size = struct.unpack_from('<II', blob, image_row + 16)
        assert size == len(image) and blob[off:off + size] == image, 'the image section is not the array'
        for name, want in (('hash', len(digest)), ('publickey', 97), ('signature', 96)):
            row = shoff + (2 + ('hash', 'publickey', 'signature').index(name)) * 40
            got_off, got_size, got_crc = struct.unpack_from('<III', blob, row + 16)[:2] + \
                (struct.unpack_from('<I', blob, row + 28)[0],)
            assert blob[got_off:got_off + got_size] == {
                'hash': digest, 'publickey': bytes(range(1, 98)),
                'signature': bytes((i * 5) & 0xff for i in range(96))}[name], name
            assert got_crc == (zlib.crc32(blob[got_off:got_off + got_size]) ^ 0xffffffff) & 0xffffffff, \
                '%s: sh_info is not the CRC32 of the section it describes' % name
        # The file the boot stub will read, judged by the checker's own rules and passing.
        checked = run(CHECKER, out, '--arch', '0x1b')
        assert checked.returncode == 0, checked.stdout + checked.stderr
        assert 'hash=%d image=%d publickey=97 signature=96  [ok]' % (len(digest), len(image)) in checked.stdout, \
            checked.stdout
        assert 'the panel will say fw 1/5 and build a handover from the FMC image' in checked.stdout, \
            checked.stdout


def test_the_driver_own_quirks_are_the_ones_the_tool_handles():
    with tempfile.TemporaryDirectory() as tmp:
        src, out = os.path.join(tmp, 'src'), os.path.join(tmp, 'out')
        os.makedirs(src)
        # 1. a 97-byte key inside a 104-byte array: the padding is not part of the entry.
        text, _image, _digest = archive(pkey=97)
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text)
        assert extract(src, out).returncode == 0
        pieces = os.path.join(out, 'pieces', 'publickey-1.2.3.bin')
        assert os.path.getsize(pieces) == 97, 'the word padding leaked into the key: %d bytes' % \
            os.path.getsize(pieces)
        # 2. a compressed entry, which is what the concatenated FMC image really is.
        text, image, _digest = archive(compress_image=True)
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text)
        result = extract(src, os.path.join(tmp, 'out2'))
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'image %d B' % len(image) in re.sub(r'\s+', ' ', result.stdout), result.stdout
        assert open(os.path.join(tmp, 'out2', 'pieces', 'image-1.2.3.bin'), 'rb').read() == image, \
            'the inflated image is not the image that was declared'
        # 3. an array shorter than its declared size is refused, in the words that say so.
        text, _image, _digest = archive(truncate=0x100)
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text)
        result = extract(src, os.path.join(tmp, 'out3'))
        assert result.returncode != 0 and 'the entry is truncated' in result.stdout + result.stderr, \
            result.stdout + result.stderr
        # 4. a hash that is not the image's digest is said as a lost claim, not a hidden one.
        text, _image, _digest = archive(hash_bytes=bytes(48))
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text)
        result = extract(src, os.path.join(tmp, 'out4'))
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'does\n' in result.stdout or 'not SHA-384 of the image' in re.sub(r'\s+', ' ', result.stdout), \
            result.stdout


def test_a_missing_archive_says_what_is_missing_rather_than_inventing_one():
    with tempfile.TemporaryDirectory() as tmp:
        src, out = os.path.join(tmp, 'src'), os.path.join(tmp, 'out')
        os.makedirs(src)
        result = extract(src, out)
        assert result.returncode != 0, 'a tool that fabricates firmware would be the worst kind of help'
        text = result.stdout + result.stderr
        assert 'g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB202.c is not in' in text, text
        assert '--fetch' in text and '--dir' in text, text
        # Three pieces of four is still not a container, and says which one is gone.
        text, _i, _d = archive()
        with open(os.path.join(src, ARCHIVE % 'GB202'), 'w') as handle:
            handle.write(text.replace('BINDATA_LABEL_UCODE_SIG")', 'BINDATA_LABEL_UCODE_SIG_OTHER")'))
        result = extract(src, out)
        assert result.returncode != 0, result.stdout
        assert 'carries 3 of the four sections' in result.stdout + result.stderr, result.stdout + result.stderr


def test_the_notice_names_where_the_bytes_came_from():
    result = run(TOOL, '--notice', '--fetch', '615.71.10', '--chip', 'GB202')
    assert result.returncode == 0, result.stdout + result.stderr
    text = result.stdout
    assert 'open-gpu-kernel-modules 615.71.10' in text and 'MIT' in text, text
    assert 'unmodified' in text, text
    # And the tool's own docstring carries the measured widths, because that is the sentence that tells a
    # reader whether their chip's archive is the shape this build can use.
    doc = Path(TOOL).read_text()
    assert 'UCODE_HASH` 48, `UCODE_PKEY` 97, `UCODE_SIG` 96' in doc, \
        'the measured numbers this exists to prove should be in its own summary'


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for _name, fn in tests:
        fn()
    print('PASS: gsp-firmware - the vendor\'s arrays, the kernel\'s container, and the hash that proves the '
          'bytes were not touched')


if __name__ == '__main__':
    main()
