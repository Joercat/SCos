#!/usr/bin/env python3
"""Deterministic GPT/ESP FAT32 image containing only native x64 UEFI startup.

No partitioning utilities, mounted filesystems, host disks or BIOS boot code.
Firmware loads EFI/BOOT/BOOTX64.EFI; that application reads SCOS/KERNEL.ELF
and its accidental-corruption CRC from the same EFI system partition.
"""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import uuid
import re
import zlib

SECTOR = 512
SECTORS = 131072  # 64 MiB, enough clusters for an actual FAT32 filesystem
DATA_FIRST, DATA_SECTORS = 129024, 256
FIRST, LAST = 2048, DATA_FIRST - 1


def validate(efi, elf, build=Path('build')):
    if len(efi) < 64 or efi[:2] != b'MZ':
        raise ValueError('missing PE image')
    pe = struct.unpack_from('<I', efi, 60)[0]
    if pe + 24 + 112 > len(efi) or efi[pe:pe+4] != b'PE\0\0':
        raise ValueError('invalid PE header')
    if (struct.unpack_from('<H', efi, pe+4)[0] != 0x8664 or
            struct.unpack_from('<H', efi, pe+24)[0] != 0x20b or
            struct.unpack_from('<H', efi, pe+24+68)[0] != 10):
        raise ValueError('loader must be AMD64 PE32+ EFI application')
    count = struct.unpack_from('<H', efi, pe+6)[0]
    optional = struct.unpack_from('<H', efi, pe+20)[0]
    section_alignment, file_alignment = struct.unpack_from('<II', efi, pe+24+32)
    image_size, headers = struct.unpack_from('<II', efi, pe+24+56)
    table = pe+24+optional
    entry = struct.unpack_from('<I', efi, pe+24+16)[0]
    if (optional < 240 or not 1 <= count <= 64 or table+40*count > headers or
            headers > len(efi) or section_alignment != 4096 or file_alignment != 512):
        raise ValueError('unsupported PE section/header layout')
    sections = []
    executable = False
    for i in range(count):
        virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', efi, table+i*40+8)
        flags = struct.unpack_from('<I', efi, table+i*40+36)[0]
        span = max(virtual_size, raw_size)
        if (not span or rva < headers or rva % 4096 or rva+span > image_size or
                (raw_size and (raw < headers or raw % 512 or raw+raw_size > len(efi))) or
                (flags & 0xa0000000) == 0xa0000000):
            raise ValueError('invalid PE section bounds/permissions')
        if any(rva < b and rva+span > a for a,b,_,_ in sections):
            raise ValueError('overlapping PE sections')
        executable |= bool(flags & 0x20000000 and rva <= entry < rva+min(virtual_size, raw_size))
        sections.append((rva,rva+span,raw,raw_size))
    def pe_bytes(rva, size):
        for start,end,raw,raw_size in sections:
            if start <= rva and rva-start+size <= raw_size:
                return efi[raw+rva-start:raw+rva-start+size]
        raise ValueError('PE directory is not file backed')
    imports, import_size = struct.unpack_from('<II', efi, pe+24+112+8)
    if import_size and any(pe_bytes(imports, import_size)):
        raise ValueError('UEFI application cannot import a Windows runtime')
    reloc, reloc_size = struct.unpack_from('<II', efi, pe+24+112+40)
    if not executable or not reloc_size:
        raise ValueError('PE must have executable entry and base relocations')
    data = pe_bytes(reloc, reloc_size)
    offset = 0
    while offset < len(data):
        if len(data)-offset < 8:
            raise ValueError('truncated PE relocation block')
        page, size = struct.unpack_from('<II', data, offset)
        if size < 8 or size % 2 or size > len(data)-offset:
            raise ValueError('invalid PE relocation block')
        for j in range(offset+8, offset+size, 2):
            fixup = struct.unpack_from('<H', data, j)[0]
            if fixup >> 12 not in (0,10):
                raise ValueError('non-native PE relocation')
            if fixup >> 12 == 10:
                pe_bytes(page+(fixup & 4095),8)
        offset += size
    if len(elf) < 64 or elf[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('kernel must be ELF64 little-endian version 1')
    _, kind, machine, version, entry, phoff, _, _, size, stride, count, _, _, _ = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
    if (kind, machine, version, entry, size, stride) != (3, 62, 1, 0, 64, 56):
        raise ValueError('invalid kernel executable/entry')
    if not 1 <= count <= 32 or phoff < 64 or phoff + count*56 > len(elf):
        raise ValueError('invalid ELF program table')
    spans = []
    entry_ok = False
    for i in range(count):
        t, f, off, va, pa, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', elf, phoff+i*56)
        if t in (3, 7):
            raise ValueError('dynamic/interpreted kernel unsupported')
        if t != 1:
            continue
        if (va != pa or pa >= 0x4000000 or pa % 4096 or
                filesz > memsz or pa+memsz > 0x4000000 or off+filesz > len(elf) or
                not f & 4 or f & ~7 or f & 3 == 3):
            raise ValueError('invalid load segment')
        if align > 1 and (align & (align-1) or (pa-off) % align):
            raise ValueError('invalid load alignment')
        end = (pa+memsz+4095) & ~4095
        if memsz:
            if any(pa < b and end > a for a, b in spans):
                raise ValueError('overlapping segment permission pages')
            spans.append((pa, end))
        entry_ok |= bool(f & 1 and pa <= entry < pa+filesz)
    if not entry_ok:
        raise ValueError('entry not executable')


def name(value):
    base, _, ext = value.partition('.')
    if len(base) > 8 or len(ext) > 3:
        raise ValueError('only short FAT names used')
    return (base.ljust(8)+ext.ljust(3)).encode('ascii')



# --------------------------------------------------------------- provenance ----
# The kernel carries build/scosbuild.h's commit, tree state and build time as plain string literals;
# this record is generated from that same header and checked against the bytes being packed, so the
# disk can never claim to hold a build it does not.  \SCOS\BUILD.TXT lands on the ESP for anyone
# holding the stick, and build.json is what the repository commits next to the image.
def stamp_values(directory):
    header = Path(directory) / 'scosbuild.h'
    if not header.exists():
        raise SystemExit('build/scosbuild.h is missing: build through the Makefile, which generates '
                         'the build stamp that the kernel and this disk record must agree on')
    text = header.read_text()
    found = dict(re.findall(r'#define SCOS_BUILD_(\w+) "([^"]*)"', text))
    missing = [f for f in ('COMMIT', 'BRANCH', 'TREE', 'DATE') if f not in found]
    if missing:
        raise SystemExit('build/scosbuild.h has no stamp for %s' % ', '.join(missing))
    return found


def provenance(directory, elf):
    v = stamp_values(directory)
    for field in ('COMMIT', 'DATE'):
        needle = v[field].encode()
        if len(needle) >= 8 and elf.count(needle) < 1:
            raise SystemExit(
                'refusing to pack this image: the build stamp says commit %s built %s, but the kernel '
                'binary does not contain that stamp. The kernel was compiled before the stamp changed - '
                'run `make` again instead of shipping an image that misreports itself.' % (v['COMMIT'], v['DATE']))
    return v


def build_record(v, elf, modules):
    lines = ['SCos x64 UEFI image - build record',
             '  build tag   : x64-dev',
             '  commit      : %s' % v['COMMIT'],
             '  branch      : %s' % v['BRANCH'],
             '  tree state  : %s' % v['TREE'],
             '  built (UTC) : %s' % v['DATE'],
             '  kernel.elf  : %d B sha256 %s' % (len(elf), hashlib.sha256(elf).hexdigest())]
    for path, blob in modules:
        lines.append('  module %-9s: %d B sha256 %s' % (path, len(blob), hashlib.sha256(blob).hexdigest()))
    if v['TREE'] != 'clean':
        lines.append('  WARNING: this image was built with uncommitted changes. Treat it as an experiment:')
        lines.append('           features it lacks, or has, say nothing about any commit in the repository.')
    lines.append('')
    return '\n'.join(lines), {'build_tag': 'x64-dev', 'commit': v['COMMIT'], 'branch': v['BRANCH'],
                             'tree': v['TREE'], 'built': v['DATE'], 'kernel_bytes': len(elf),
                             'kernel_sha256': hashlib.sha256(elf).hexdigest(),
                             'modules': [{'name': n, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                                         for n, b in modules]}


def main(directory):
    d = Path(directory)
    efi = (d/'BOOTX64.EFI').read_bytes()
    elf = (d/'kernel.elf').read_bytes()
    validate(efi, elf)
    image = bytearray(SECTORS*SECTOR)
    # Protective MBR is metadata, not an executable BIOS loader.
    image[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff'*3, 1, SECTORS-1)
    image[510:512] = b'\x55\xaa'
    image[400:412] = b'SCOSDATA64v1'
    struct.pack_into('<III', image, 412, DATA_FIRST, DATA_SECTORS, SECTORS)
    partition_id = uuid.UUID('b2a52b42-b792-4e85-9ce6-5e9bdcd425a7').bytes_le
    disk_id = uuid.UUID('1cb5b534-9ee8-4a50-b9d5-600ce619b86c').bytes_le
    entries = bytearray(128*128)
    entries[:128] = struct.pack('<16s16sQQQ72s', uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le,
                               partition_id, FIRST, LAST, 0, 'SCos EFI system'.encode('utf-16le'))
    entries[128:256] = struct.pack('<16s16sQQQ72s', uuid.UUID('1ffcb787-a904-4fce-8fbd-c728ac57e333').bytes_le,
                                   uuid.UUID('1f5b84d1-9560-4345-9f49-74bc872143e4').bytes_le,
                                   DATA_FIRST, DATA_FIRST+DATA_SECTORS-1, 0, 'SCos data'.encode('utf-16le'))
    for current, backup, table in ((1, SECTORS-1, 2), (SECTORS-1, 1, SECTORS-33)):
        header = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0,
                                      current, backup, 34, SECTORS-34, disk_id, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', header, 16, zlib.crc32(header))
        image[current*512:current*512+92] = header
        image[table*512:table*512+len(entries)] = entries
    total = LAST-FIRST+1
    fat_sectors = (total-32+2+129)//130  # one sector/cluster, two FAT copies
    data_sector = FIRST+32+2*fat_sectors
    clusters = total-32-2*fat_sectors
    if not 65525 <= clusters < 0x0ffffff5:
        raise ValueError('invalid FAT32 cluster count')
    boot = bytearray(512)
    boot[:11] = b'\xeb\x58\x90SCOSUEFI'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, 1, 32, 2, 0, 0, 0xf8, 0, 63, 255, FIRST, total)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64] = 0x80; boot[66] = 0x29
    struct.pack_into('<I', boot, 67, 0x53434f53)
    boot[71:82] = b'SCOS UEFI  '; boot[82:90] = b'FAT32   '; boot[510:] = b'\x55\xaa'
    info = bytearray(512)
    struct.pack_into('<I', info, 0, 0x41615252);struct.pack_into('<III', info, 484, 0x61417272, 0xffffffff, 0xffffffff)
    struct.pack_into('<I', info, 508, 0xaa550000)
    for offset, contents in ((0, boot), (6, boot), (1, info), (7, info)):
        image[(FIRST+offset)*512:(FIRST+offset+1)*512] = contents
    fat = bytearray(fat_sectors*512)
    struct.pack_into('<II', fat, 0, 0x0ffffff8, 0x0fffffff)
    next_cluster = 2
    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data)+511)//512)
        first = next_cluster
        if first+count > clusters+2:
            raise ValueError('ESP capacity exceeded')
        for c in range(first, first+count):
            struct.pack_into('<I', fat, c*4, c+1 if c+1 < first+count else 0x0fffffff)
        offset = (data_sector+first-2)*512
        image[offset:offset+len(data)] = data
        next_cluster += count
        return first
    def entry(filename, cluster, size=0, directory=False):
        data = bytearray(32);data[:11] = name(filename);data[11] = 16 if directory else 32
        struct.pack_into('<H', data, 16, 0x21);struct.pack_into('<H', data, 18, 0x21)
        struct.pack_into('<H', data, 20, cluster >> 16);struct.pack_into('<H', data, 24, 0x21)
        struct.pack_into('<HI', data, 26, cluster & 65535, size)
        return data
    dirs = [allocate(bytes(512)) for _ in range(4)]  # root, EFI, BOOT, SCOS
    ec, kc, cc = allocate(efi), allocate(elf), allocate(struct.pack('<I', zlib.crc32(elf)))
    def dot(parent, own):
        a = entry('DOT', own, directory=True);a[:11] = b'.          '
        b = entry('DOT', parent, directory=True);b[:11] = b'..         '
        return a+b
    # GPU display driver modules: one file per family, kept apart so the boot stub reads only the
    # module this machine's chip names.  The index is a fixed-size table, not a directory walk, so
    # the firmware can report how many modules exist without opening any of them.
    module_dir = Path(directory, 'gpu')
    modules = []
    for path in sorted(module_dir.glob('*.mod')) if module_dir.is_dir() else []:
        blob = path.read_bytes()
        if len(blob) > 256*1024:
            raise ValueError('GPU module %s exceeds the module region' % path.name)
        modules.append((path.stem.upper()[:8], blob))
    index = struct.pack('<II', len(modules), sum(len(blob) for _n, blob in modules))
    for stem, blob in modules:
        index += struct.pack('<8sII', stem.encode()[:8].ljust(8, b'\0'), len(blob),
                             zlib.crc32(blob) & 0xffffffff)
    if len(index) > 4096:
        raise ValueError('driver index too large')
    ic = allocate(index)
    v = provenance(directory, elf)
    # The record names the modules as the ESP holds them (FAT is upper-case), which is what a reader
    # checking the stick with another OS will see, so the two must not use different spellings.
    record_text, record_json = build_record(v, elf,
                                            [(stem + '.MOD', blob) for stem, blob in modules])
    (d / 'BUILD.TXT').write_text(record_text)
    (d / 'build.json').write_text(json.dumps(record_json, sort_keys=True, indent=1) + '\n')
    bc = allocate(record_text.encode())
    scos = (dot(0, dirs[3]) + entry('KERNEL.ELF', kc, len(elf)) + entry('KERNEL.CRC', cc, 4)
            + entry('BUILD.TXT', bc, len(record_text.encode())) + entry('DRVLIST.IDX', ic, len(index)))
    for stem, blob in modules:
        scos += entry(stem + '.MOD', allocate(blob), len(blob))
    directory_data = [entry('EFI', dirs[1], directory=True)+entry('SCOS', dirs[3], directory=True),
                      dot(0, dirs[1])+entry('BOOT', dirs[2], directory=True),
                      dot(dirs[1], dirs[2])+entry('BOOTX64.EFI', ec, len(efi)),
                      scos]
    for c, data in zip(dirs, directory_data):
        offset = (data_sector+c-2)*512;image[offset:offset+len(data)] = data
    for i in range(2):
        offset = (FIRST+32+i*fat_sectors)*512;image[offset:offset+len(fat)] = fat
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=d, prefix='.uefi-', delete=False) as f:
            temporary = Path(f.name);f.write(image)
        temporary.replace(d/'scos.img')
    finally:
        if temporary is not None:temporary.unlink(missing_ok=True)
    print(f'x64 UEFI GPT/ESP: {len(image)} bytes; loader {len(efi)}; ELF {len(elf)}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv)>1 else 'build')
