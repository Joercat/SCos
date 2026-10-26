#!/usr/bin/env python3
r"""Rebuild the FMC image this OS hands to the FSP, from the bytes the driver itself ships.

Why this exists: the panel can only say what it can read, and the file a handover is built from -
`/lib/firmware/nvidia/<chip>/gsp/fmc-<version>.bin`, which nouveau calls `NVKM_GSP_FIRMWARE_FMC` - lives on
whoever's machine already has the driver.  It does not have to: the open kernel module ships the same image
inside the driver, as generated C arrays in `src/nvidia/generated/g_bindata_kgspGetBinArchive*.c`, under MIT.
For a Blackwell GFW archive the arrays are the four pieces a container is made of, and their sizes are the
widths the FSP is told in `NVDM_PAYLOAD_COT`: measured on 615.71.10 for GB202 (which is the family a GB207
uses), `UCODE_HASH` 48, `UCODE_PKEY` 97, `UCODE_SIG` 96, `UCODE_IMAGE` 287304.  So this reads those arrays,
checks each one against the size its own header declares (and inflates the ones it is told are compressed), and
wraps the four into the ELF32 container the kernel's parser and nouveau's `gh100_gsp_oneinit()` both insist on:
the byte-exact 52-byte header, six section headers, the name table at index 1, `sh_info` holding CRC32 of each
section's bytes, sections named hash/publickey/signature/image in that order.

Nothing here changes a byte of NVIDIA's image: the payload the FSP authenticates is the array as shipped, and
the container around it is what the signature does not cover - the coprocessor is given the four buffers, not
the file.  The hash is checked here anyway, against the image, because if `hash' is SHA-384 of `image' then the
sentence "this is the same content the driver boots" stops being an inference.

Provenance to disclose when the output goes onto a disk: the arrays are from
https://github.com/NVIDIA/open-gpu-kernel-modules, tag as passed to --fetch (or the checkout --dir points into),
license MIT, files generated from `kernel/inc/gspcc/bin/*.bin'.  `--notice' prints the text this OS's docs use.

    python3 tools/gsp-firmware.py --fetch 615.71.10 --chip gb202 --out build/gsp
    python3 tools/gsp-firmware.py --dir /path/to/open-gpu-kernel-modules/src/nvidia/generated \
            --version 615.71.10 --chip gb202 --out build/gsp
    python3 tools/gsp-firmware.py --dir build/gsp/src --version 615.71.10 --out build/gsp && make dist
"""

import argparse
import glob
import hashlib
import os
import re
import struct
import subprocess
import sys
import zlib

ARCHIVE = 'g_bindata_kgspGetBinArchive{kind}_{chip}.c'
# The four labels the GFW archive carries, and the section name each one becomes in the container.
GFW_LABELS = (('UCODE_HASH', 'hash'), ('UCODE_PKEY', 'publickey'), ('UCODE_SIG', 'signature'),
              ('UCODE_IMAGE', 'image'))
FMC_HEADER = bytes([
    0x7f, 0x45, 0x4c, 0x46, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    52, 0, 0, 0, 0, 0, 0, 0,
    52, 0, 0, 0,
    0, 0, 40, 0,
    6, 0, 1, 0,
])
BLOCK = re.compile(r'// FUNCTION: (\w+)\("BINDATA_LABEL_(\w+)"\)\n(.*?)\nstatic BINDATA_CONST[^\n]*?\n\{([^}]*)\};',
                   re.S)
NOTICE = ('GPU firmware in this image: rebuilt from the generated bindata arrays of '
          'NVIDIA open-gpu-kernel-modules %s (chip %s), license MIT, original files '
          'kernel/inc/gspcc/bin/*.bin as published by NVIDIA. The image bytes are unmodified; only the '
          'container around them is this build\'s.')


class Refused(Exception):
    pass


def words(text):
    """The NvU64 literals of one array, in order, as bytes."""
    out = bytearray()
    for value in re.findall(r'0x([0-9a-fA-F]{1,16})ULL', text):
        out += struct.pack('<Q', int(value, 16))
    return bytes(out)


def inflate(data, want):
    """The driver inflates a compressed entry with utilGzGetData; the framing is not named in the generated
    comment, so all three that zlib accepts are tried and the one that yields exactly the declared size wins.
    Anything else would be a guess about someone else's file."""
    for how in ('raw', 'zlib', 'gzip'):
        try:
            wbits = {'raw': -15, 'zlib': 15, 'gzip': 31}[how]
            got = zlib.decompressobj(wbits).decompress(data, want + 1)
        except zlib.error:
            continue
        if len(got) == want:
            return how, got
    raise Refused('no zlib framing inflates these %d bytes to the declared %d; the entry is not what this '
                  'tool can rebuild' % (len(data), want))


def entries(path):
    """Every label one generated archive file carries, with its declared sizes and its bytes."""
    source = open(path, 'r', encoding='utf-8', errors='replace').read()
    found = {}
    for _fn, label, header, array in BLOCK.findall(source):
        want = re.search(r'DATA SIZE \(bytes\): (\d+)', header)
        have = re.search(r'COMPRESSED SIZE \(bytes\): (\d+)', header)
        if not want:
            continue
        want, have = int(want.group(1)), int(have.group(1)) if have else None
        raw = words(array)
        compressed = 'COMPRESSION: YES' in header
        # An uncompressed entry is the data followed by whatever padding the NvU64 array needs - the public key
        # is declared as 97 bytes and occupies 13 words, 104 - so the declared size bounds the entry, and an
        # array shorter than that is the only real inconsistency.
        if compressed:
            _framing, blob = inflate(raw[:have] if have else raw, want)
        elif len(raw) < want:
            raise Refused('%d bytes of array for a declared %d: the entry is truncated' % (len(raw), want))
        else:
            blob = raw[:want]
        if len(blob) != want:
            raise Refused('%s: %s holds %d bytes after %s and the header declares %d - the file is not '
                          'consistent with itself' % (os.path.basename(path), label, len(blob),
                                                      'inflation' if compressed else 'the array', want))
        found[label] = {'label': label, 'bytes': blob, 'declared': want, 'compressed': compressed,
                        'file': (re.search(r'FILE NAME: (\S+)', header) or [None, ''])[1],
                        'crc32': (zlib.crc32(blob) ^ 0xffffffff) & 0xffffffff}
    return found


def container(parts, order):
    """The ELF32 container, laid out exactly as the kernel's parser reads it: header, six section headers, the
    name table, then the four payloads in the order the FSP is told about them, each CRC32 parked in sh_info."""
    strings = b'\0' + b''.join(n.encode() + b'\0' for n in order)
    shoff, shentsize, count = len(FMC_HEADER), 40, 6
    at = shoff + shentsize * count + len(strings)
    offsets = {}
    for name in order:
        offsets[name] = (at, len(parts[name]))
        at += len(parts[name])
    blob = bytearray(FMC_HEADER)
    blob += bytes(at - len(FMC_HEADER))
    for name in order:
        off, size = offsets[name]
        blob[off:off + size] = parts[name]
    rows = struct.pack('<10I', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    rows += struct.pack('<10I', 0, 3, 0x20, 0, shoff + shentsize * count, len(strings), 0, 0, 1, 0)
    for index, name in enumerate(order):
        off, size = offsets[name]
        rows += struct.pack('<10I', 1 + sum(len(n) + 1 for n in order[:index]), 1, 0xfff00102, 0, off, size,
                            0, (zlib.crc32(parts[name]) ^ 0xffffffff) & 0xffffffff, 1, 0)
    blob[shoff:shoff + len(rows)] = rows
    blob[shoff + shentsize * count:shoff + shentsize * count + len(strings)] = strings
    return bytes(blob)


def fetch(ref, chip, into):
    """Pull the generated archives for one chip out of the public repository, without an npm lifecycle or a
    clone: the contents API for what is small enough for it and the blob API for what is not."""
    os.makedirs(into, exist_ok=True)
    kinds = ['GspRmFmcGfwProdSigned', 'ConcatenatedFMC', 'ConcatenatedFMCDesc', 'GspRmBoot']
    written = []
    for kind in kinds:
        path = ARCHIVE.format(kind=kind, chip=chip.upper())
        dest = os.path.join(into, path)
        if os.path.exists(dest) and os.path.getsize(dest):
            written.append(dest)
            continue
        # The blob API, always, and never the contents API's inline base64: the contents endpoint answers a
        # request for a file above about a megabyte with a `.content' that is empty or cut short, and it does so
        # with exit status 0, which is how a 4 MB array became a "not the generated file" complaint here once
        # already.  Asking for the object id and then the object is one call slower and cannot trunc
        # an answer, and these files are 1 to 4 MB each.
        api = 'repos/NVIDIA/open-gpu-kernel-modules/contents/src/nvidia/generated/%s?ref=%s' % (path, ref)
        try:
            sha = subprocess.run(['gh', 'api', api, '--jq', '.sha'], capture_output=True, text=True)
            sha = sha.stdout.strip()
            if not re.fullmatch(r'[0-9a-f]{40}', sha):
                raise Refused('%s: gh api gave no object id (%s)' % (path, sha[:120]))
            blob = subprocess.run(['gh', 'api', 'repos/NVIDIA/open-gpu-kernel-modules/git/blobs/' + sha,
                                   '--jq', '.content'], capture_output=True, text=True)
            # base64 -d takes bytes, and gh hands back text: passing a str through was a TypeError that only this
            # --fetch path could reach, which is how it survived a suite that reads --dir.
            data = subprocess.run(['base64', '-d'], input=blob.stdout.encode(), capture_output=True).stdout
            if not data.strip():
                raise Refused('%s: the blob came back empty' % path)
            if b'BINDATA_CONST' not in data and b'g_bindata_' not in data:
                raise Refused('%s: %d bytes came back and none of them look like a generated bindata array'
                              % (path, len(data)))
        except FileNotFoundError:
            # No gh: the public raw URL works anywhere with network.
            url = ('https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/%s/src/nvidia/generated/%s'
                   % (ref, path))
            data = subprocess.run(['curl', '-fsSL', '--max-time', '300', url], capture_output=True).stdout
            if not data:
                raise Refused('%s: neither gh nor curl could fetch it; pass --dir pointing at a checkout of '
                              'open-gpu-kernel-modules %s instead' % (path, ref))
        with open(dest, 'wb') as handle:
            handle.write(data)
        written.append(dest)
    return written


def build(src_dir, version, chip, out_dir, kinds=('GspRmFmcGfwProdSigned',)):
    report = {'version': version, 'chip': chip, 'files': {}, 'hash_matches_image': None, 'notes': []}
    parts, order = {}, []
    for kind in kinds:
        path = os.path.join(src_dir, ARCHIVE.format(kind=kind, chip=chip.upper()))
        if not os.path.exists(path):
            raise Refused('%s is not in %s: fetch it (--fetch %s) or point --dir at a checkout of the open '
                          'kernel module' % (ARCHIVE.format(kind=kind, chip=chip.upper()), src_dir, version))
        found = entries(path)
        for label, name in GFW_LABELS:
            if label in found:
                parts[name] = found[label]['bytes']
                order.append(name)
                report['files'][name] = {'label': label, 'bytes': len(found[label]['bytes']),
                                         'crc32': '0x%08x' % found[label]['crc32'],
                                         'compressed': found[label]['compressed'],
                                         'source': os.path.basename(found[label]['file'])
                                         if found[label]['file'] else ''}
    if len(order) != 4:
        raise Refused('%s carries %d of the four sections this container needs (%s); the missing ones are not '
                      'in the driver\'s generated arrays, so the file has to come off a machine with the '
                      'driver installed' % (ARCHIVE.format(kind=kinds[0], chip=chip.upper()), len(order),
                                            ', '.join(sorted(order))))
    digest = hashlib.sha384(parts['image']).digest()
    report['hash_matches_image'] = 'sha384' if digest == parts['hash'] else None
    if report['hash_matches_image'] is None:
        report['notes'].append('the 48-byte hash is not SHA-384 of the image as stored here, so this tool does '
                               'not claim to know what the coprocessor hashes; the bytes are still the driver\'s')
    blob = container(parts, order)
    # `--out' names either a directory to write fmc-<version>.bin into (what `make gsp-firmware' passes) or the
    # file itself.  Both readings are unambiguous, and refusing one of them would only mean a second flag.
    dest = out_dir if out_dir.endswith('.bin') else out_dir + '/fmc-%s.bin' % version
    os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
    with open(dest, 'wb') as handle:
        handle.write(blob)
    report['container'] = {'path': dest, 'bytes': len(blob), 'crc32': '0x%08x' % ((zlib.crc32(blob) ^
                                                                                 0xffffffff) & 0xffffffff)}
    pieces = os.path.join(os.path.dirname(dest) or '.', 'pieces')
    os.makedirs(pieces, exist_ok=True)
    for name in order:
        with open(os.path.join(pieces, '%s-%s.bin' % (name, version)), 'wb') as handle:
            handle.write(parts[name])
    report['pieces'] = pieces
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dir', help='directory holding the generated g_bindata_*.c files')
    ap.add_argument('--fetch', metavar='REF', help='tag or branch of NVIDIA/open-gpu-kernel-modules to fetch '
                                                   'the arrays from (gh api, or curl)')
    ap.add_argument('--chip', default='GB202', help='the chip the driver names for your card (gb207 ships as '
                                                    'GB202 arrays; see nvkm/subdev/gsp/gb202.c)')
    ap.add_argument('--version', default='615.71.10', help='the driver version the arrays came from, used in '
                                                           'the file name the ESP is asked to carry')
    ap.add_argument('--out', default='build/gsp', help='where fmc-<version>.bin goes; make dist packs it')
    ap.add_argument('--notice', action='store_true', help='print the provenance text and stop')
    args = ap.parse_args()
    if args.notice:
        print(NOTICE % (args.fetch or args.dir, args.chip))
        return 0
    src = args.dir
    if args.fetch:
        src = os.path.join(args.out, 'src')
        for path in fetch(args.fetch, args.chip, src):
            print('have %s (%d B)' % (os.path.basename(path), os.path.getsize(path)))
    if not src or not os.path.isdir(src):
        raise SystemExit('gsp-firmware: --dir must point at the generated arrays (or pass --fetch TAG); '
                         'nothing is invented here, so there is no third option')
    try:
        rep = build(src, args.version, args.chip, args.out)
    except Refused as e:
        raise SystemExit('gsp-firmware: %s' % e)
    print('gsp-firmware: %s from %s' % (os.path.basename(rep['container']['path']), src))
    for name, info in sorted(rep['files'].items()):
        print('  %-10s %7d B  crc32 %s  %s%s' % (name, info['bytes'], info['crc32'], info['label'],
                                                  '  (inflated)' if info['compressed'] else ''))
    print('  container  %7d B  crc32 %s -> %s' % (rep['container']['bytes'], rep['container']['crc32'],
                                                   rep['container']['path']))
    if rep['hash_matches_image']:
        print('  the hash section is %s of the image section, byte for byte: this is the content the '
              'coprocessor authenticates' % rep['hash_matches_image'])
    for note in rep['notes']:
        print('  note: ' + note)
    print('  next: python3 tools/gspcheck.py %s && make dist' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
