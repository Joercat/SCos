#!/usr/bin/env python3
"""Date a USB stick or image from the bytes it holds, without booting it.

Why this exists: a report from a machine that ran a *previous* image reads exactly like a report from a
machine running the current one, and the only difference is a handful of sentences - which is how one boot
cycle got spent on a fix the stick never carried.  This tool reads the disk's own record: the GPT/ESP FAT32
layout this repository packs, `\SCOS\BUILD.TXT`, which `tools/makedisk.py` generates from the same build
stamp the kernel prints and checks against the bytes it packed.  It reads a file or a block device, opens it
read-only, and never writes anywhere.

    python3 tools/verify-stick.py /dev/sdb
    python3 tools/verify-stick.py dist/scos.img --against dist/scos.img.build.json
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

SECTOR = 512
SCAN_LIMIT = 72 * 1024 * 1024          # the ESP can start well into the image; a 64 MiB image fits in this


def find_fat32_boot_sector(read):
    """Locate the FAT32 boot sector by its own signature rather than a hard-coded partition offset.

    The offset is deliberately not assumed: an image file, a whole device and a partition written to a
    stick differ in where the ESP begins, and a tool meant to tell a good stick from an old one must not
    depend on the thing it is checking.
    """
    for base in range(0, SCAN_LIMIT, SECTOR):
        sector = read(base, SECTOR)
        if len(sector) < SECTOR:
            return None
        if sector[82:90] != b'FAT32   ' or sector[510:512] != b'\x55\xaa':
            continue
        return base, sector
    return None


def parse_bpb(sector):
    # Offsets are the FAT32 extended BPB itself: BytesPerSec 0x0B, SecPerClus 0x0D, RsvdSecCnt 0x0E,
    # NumFATs 0x10, FATSz32 0x24, RootClus 0x2C.  They are spelled in hex with the field named because
    # counting bytes from 0x0B is how a tool ends up reading the backup-boot-sector pointer as a FAT length
    # and reporting "no \SCOS directory" about a perfectly good stick.
    bytes_per_sector, sectors_per_cluster = struct.unpack_from('<HB', sector, 0x0B)
    reserved, fats = struct.unpack_from('<HB', sector, 0x0E)
    fat_size32 = struct.unpack_from('<I', sector, 0x24)[0]
    root_cluster = struct.unpack_from('<I', sector, 0x2C)[0]
    if bytes_per_sector not in (512, 1024, 2048, 4096) or not sectors_per_cluster:
        raise ValueError('FAT32 boot sector disagrees with itself')
    return dict(bytes_per_sector=bytes_per_sector, sectors_per_cluster=sectors_per_cluster,
                reserved=reserved, fats=fats, fat_size32=fat_size32, root_cluster=root_cluster)


def cluster_offset(read, base, bpb, cluster):
    """Byte offset of a cluster: one sector per cluster is what makedisk writes, and anything else is
    computed rather than assumed so a future layout change shows up as a bad name, not a silent zero."""
    first_data = base + bpb['reserved'] * bpb['bytes_per_sector'] + bpb['fats'] * bpb['fat_size32'] * bpb['bytes_per_sector']
    sectors_per_cluster = bpb['sectors_per_cluster']
    return first_data + (cluster - 2) * sectors_per_cluster * bpb['bytes_per_sector']


def read_cluster_chain(read, base, bpb, cluster, limit=64 * 1024 * 1024):
    """Follow the FAT until it says stop.  A cycle is an error, not a loop: an old stick that confuses this
    tool must fail loudly rather than spin."""
    fat_start = base + bpb['reserved'] * bpb['bytes_per_sector']
    out = bytearray()
    seen = set()
    while 2 <= cluster < 0x0ffffff8:
        if cluster in seen:
            raise ValueError('FAT cycle at cluster %d' % cluster)
        seen.add(cluster)
        entry = struct.unpack_from('<I', read(fat_start + cluster * 4, 4), 0)[0] & 0x0fffffff
        data = read(cluster_offset(read, base, bpb, cluster), bpb['sectors_per_cluster'] * bpb['bytes_per_sector'])
        out += data
        if len(out) > limit:
            raise ValueError('cluster chain too long')
        cluster = entry
    return bytes(out)


def directory(blob):
    entries = []
    for off in range(0, len(blob) - 31, 32):
        raw = blob[off:off + 32]
        if raw[0] == 0x00:
            break
        if raw[0] in (0xe5, 0xf5) or raw[11] == 0x0f:      # deleted, or a long-name record
            continue
        name = raw[8:11].decode('ascii', 'replace')
        stem = raw[0:8].decode('ascii', 'replace').rstrip()
        cluster = struct.unpack_from('<H', raw, 20)[0] << 16 | struct.unpack_from('<H', raw, 26)[0]
        size = struct.unpack_from('<I', raw, 28)[0]
        entries.append(dict(stem=stem, ext=name.strip(), cluster=cluster, size=size,
                            directory=bool(raw[11] & 0x10)))
    return entries


def read_record(path):
    """Return (build record text, file listing of \\SCOS) or raise with the reason it could not be had."""
    handle = open(path, 'rb')
    try:
        def read(offset, count):
            handle.seek(offset)
            return handle.read(count)

        found = find_fat32_boot_sector(read)
        if not found:
            raise ValueError('no FAT32 boot sector in the first %d MiB: this is not a SCos ESP' %
                             (SCAN_LIMIT // (1024 * 1024)))
        base, sector = found
        bpb = parse_bpb(sector)
        root = read_cluster_chain(read, base, bpb, bpb['root_cluster'])
        entries = directory(root)
        scos = next((e for e in entries if e['stem'] == 'SCOS' and e['directory']), None)
        if not scos:
            raise ValueError('the ESP holds no \\SCOS directory')
        blob = read_cluster_chain(read, base, bpb, scos['cluster'])
        listing = [(e['stem'] + ('.' + e['ext'] if e['ext'] else ''), e['size']) for e in directory(blob)
                   if not e['directory']]
        record = next((e for e in directory(blob) if e['stem'] == 'BUILD' and e['ext'] == 'TXT'), None)
        if not record:
            raise ValueError('\\SCOS exists but holds no BUILD.TXT: that file was added when image provenance '
                             'became something a user could check, so its absence dates this stick')
        offset = cluster_offset(read, base, bpb, record['cluster'])
        text = read(offset, record['size']).decode('utf-8', 'replace') if record['size'] <= 1 << 20 else ''
        if not text:
            raise ValueError('BUILD.TXT is empty or too large to be the record')
        return text, listing
    finally:
        handle.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('device', help='image file or block device to read, never write')
    ap.add_argument('--against', type=Path, help='a scos.img.build.json to compare the record with')
    args = ap.parse_args()

    text, listing = read_record(args.device)
    # Labels contain spaces and parentheses (`built (UTC)'), so the key is everything before the colon.
    values = dict(re.findall(r'^\s+([^:]+?)\s*:\s*(.+)$', text, re.M))
    print(text.rstrip())
    print('  files on \\SCOS  : ' + ', '.join('%s (%d B)' % (n, s) for n, s in sorted(listing)))
    if Path(args.device).is_file():
        digest = hashlib.sha256(Path(args.device).read_bytes()).hexdigest()
        print('  image sha256  : %s' % digest)
    failures = []
    if args.against:
        want = json.loads(args.against.read_text())
        for key, field in (('commit', 'commit'), ('built (UTC)', 'built')):
            got, expected = values.get(key, ''), want.get(field, '')
            if got != expected:
                failures.append('%s: the disk says %r, the repository says %r' % (key, got, expected))
        for module in want.get('modules', []):
            name = module['name'].upper()
            size = dict(listing).get(name)
            if size != module['bytes']:
                failures.append('%s: %s B on the disk, %s B packed' % (name, size, module['bytes']))
        if failures:
            print('MISMATCH:\n  ' + '\n  '.join(failures))
            return 1
        print('MATCH: the disk record and the repository record are the same build')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print('cannot read a SCos build record from this device: %s' % exc, file=sys.stderr)
        sys.exit(2)
