#!/usr/bin/env python3
"""Deterministic GPT/ESP FAT32 image containing only native x64 UEFI startup.

No partitioning utilities, mounted filesystems, host disks or BIOS boot code.
Firmware loads EFI/BOOT/BOOTX64.EFI; that application reads SCOS/KERNEL.ELF
and its accidental-corruption CRC from the same EFI system partition.
"""
from pathlib import Path
import hashlib
import json
import os
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
# The image tracked in the repository has to stay small enough for a git host to accept it, so a build that
# needs to carry a file the 64 MiB volume cannot hold - the resource-manager image is 63571696 bytes by itself -
# is packed to a whole USB stick instead: the same writer, the same layout, the ESP grown to the device.
# `tools/flash.sh' passes the size; nothing here depends on it being 64 MiB.
GPT_BACKUP_SECTORS = 33


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


# Coprocessor firmware: the five images the open driver loads to start a Turing-or-later NVIDIA GSP.
# boot/uefi/gsp_fw.h carries why there are five and what each one is for; this table is the same list in the
# only two forms a person is likely to have: the short names the ESP holds, and the upstream file names an
# unpacked driver package ships.  Everything here is optional, and absence is not an error - a disk without
# them boots the same image, and the panel says which roles the volume does not have.
FIRMWARE_ROLES = (("gsp", "GSP.BIN", "gsp"), ("fmc", "GSPFMC.BIN", "fmc"),
                  ("bootloader", "GSPBL.BIN", "bootloader"),
                  ("booter load", "GSPBTL.BIN", "booter_load"),
                  ("booter unload", "GSPBTU.BIN", "booter_unload"))
# The boot stub cannot read past this, so a bigger file is refused at build time instead of arriving on the
# panel as "unreadable" with no size attached to it.
# The boot stub's own bound (BOOT_FIRMWARE_FILE_MAX in kernel/include/boot.h), duplicated here on purpose so a
# volume can never be packed with a file the stub would refuse to read.  It used to be 16 MiB on both sides,
# which is under the size of the resource-manager image this generation loads (gsp-570.144.bin is 63571696 B),
# so that file could not have been carried at any price.
FIRMWARE_MAX = 72 * 1024 * 1024


_ESP_READER = None


def esp_reader():
    '''The ESP reader lives in tools/verify-stick.py, which is also what dates a stick after it has been
    written; the hyphen in that file's name is why it comes in by path instead of by import.'''
    global _ESP_READER
    if _ESP_READER is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location('scos_verify_stick',
                                                     Path(__file__).resolve().parent / 'verify-stick.py')
        _ESP_READER = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_ESP_READER)
    return _ESP_READER


def firmware_blobs(directory, sub='gsp'):
    """The staged firmware, as (ESP name, bytes) pairs in role order.  build/gsp is searched recursively, so
    copying a whole linux-firmware nvidia/ tree under it works without renaming anything; a role matched by
    more than one file is refused rather than guessed at, because quietly choosing one chip image out of
    several is exactly the mistake this directory exists to make impossible."""
    root = Path(directory) if sub is None else Path(directory, sub)
    blobs = []
    if not root.is_dir():
        return blobs
    for role, esp, upstream in FIRMWARE_ROLES:
        exact = root / esp
        if exact.is_file():
            blob = exact.read_bytes()
        else:
            matches = sorted(p for p in root.rglob(upstream + "-*.bin") if p.is_file())
            if len(matches) > 1:
                raise ValueError("firmware role %s has %d candidate files (%s); leave only the image for "
                                 "the chip in the machine being booted"
                                 % (role, len(matches), ", ".join(m.name for m in matches)))
            if not matches:
                continue
            blob = matches[0].read_bytes()
        if not blob:
            raise ValueError("firmware role %s staged as %s is empty" % (role, esp))
        if len(blob) > FIRMWARE_MAX:
            raise ValueError("firmware %s is %d bytes, past the boot stub read cap of %d"
                             % (esp, len(blob), FIRMWARE_MAX))
        blobs.append((esp, blob))
    return blobs


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


def build_record(v, elf, modules, firmware=()):
    lines = ['SCos x64 UEFI image - build record',
             '  build tag   : x64-dev',
             '  commit      : %s' % v['COMMIT'],
             '  branch      : %s' % v['BRANCH'],
             '  tree state  : %s' % v['TREE'],
             '  built (UTC) : %s' % v['DATE'],
             '  kernel.elf  : %d B sha256 %s' % (len(elf), hashlib.sha256(elf).hexdigest())]
    for path, blob in modules:
        lines.append('  module %-9s: %d B sha256 %s' % (path, len(blob), hashlib.sha256(blob).hexdigest()))
    # Named in the record next to the modules, because the absence is as much a fact about the image as a
    # presence: an image packed while build/gsp happened to be empty carries no coprocessor firmware, boots,
    # and reports fw 0/5 - which reads like a missing file on somebody else's disk unless the record says it.
    for esp, blob in firmware:
        lines.append('  firmware %-9s: %d B sha256 %s' % (esp, len(blob), hashlib.sha256(blob).hexdigest()))
    if not firmware:
        lines.append('  firmware   : none staged.  This volume cannot start the coprocessor; run '
                     '`make gsp-firmware' + chr(39) + ' and repack (the dist rule now does both)')
    if v['TREE'] != 'clean':
        lines.append('  WARNING: this image was built with uncommitted changes. Treat it as an experiment:')
        lines.append('           features it lacks, or has, say nothing about any commit in the repository.')
    lines.append('')
    return '\n'.join(lines), {'build_tag': 'x64-dev', 'commit': v['COMMIT'], 'branch': v['BRANCH'],
                             'tree': v['TREE'], 'built': v['DATE'], 'kernel_bytes': len(elf),
                             'kernel_sha256': hashlib.sha256(elf).hexdigest(),
                             'modules': [{'name': n, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                                         for n, b in modules],
                             'firmware': [{'name': n, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                                          for n, b in firmware]}


def main(directory, sectors=None, firmware_dir=None, out=None):
    """Pack `directory' into a GPT/ESP image.

    `sectors' sizes the whole disk and grows the ESP into everything between the GPT and the data partition,
    `firmware_dir' names a directory of firmware to stage instead of build/gsp, and `out' says where the bytes
    go.  All three default to what the Makefile has always asked for, so `makedisk.py build' still reproduces
    the tracked 64 MiB image byte for byte; they exist because the 64 MiB volume physically cannot hold the
    resource-manager image this generation boots from, and the answer to that is a bigger disk, not a smaller
    firmware."""
    d = Path(directory)
    total_sectors = SECTORS if sectors is None else int(sectors) // SECTOR
    if total_sectors < SECTORS:
        raise ValueError('a %d-byte image cannot hold this build; the ESP has to be at least %d bytes and the '
                         'boot loader, kernel, modules and firmware all land inside it'
                         % (total_sectors * SECTOR, SECTORS * SECTOR))
    data_sectors = DATA_SECTORS
    DATA_FIRST = total_sectors - GPT_BACKUP_SECTORS - 1 - data_sectors
    FIRST, LAST = 2048, DATA_FIRST - 1
    target = d / 'scos.img' if out is None else Path(out)
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    efi = (d/'BOOTX64.EFI').read_bytes()
    elf = (d/'kernel.elf').read_bytes()
    validate(efi, elf)
    image = bytearray(total_sectors*SECTOR)
    # Protective MBR is metadata, not an executable BIOS loader.
    image[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff'*3, 1, total_sectors-1)
    image[510:512] = b'\x55\xaa'
    image[400:412] = b'SCOSDATA64v1'
    struct.pack_into('<III', image, 412, DATA_FIRST, data_sectors, total_sectors)
    partition_id = uuid.UUID('b2a52b42-b792-4e85-9ce6-5e9bdcd425a7').bytes_le
    disk_id = uuid.UUID('1cb5b534-9ee8-4a50-b9d5-600ce619b86c').bytes_le
    entries = bytearray(128*128)
    entries[:128] = struct.pack('<16s16sQQQ72s', uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le,
                               partition_id, FIRST, LAST, 0, 'SCos EFI system'.encode('utf-16le'))
    entries[128:256] = struct.pack('<16s16sQQQ72s', uuid.UUID('1ffcb787-a904-4fce-8fbd-c728ac57e333').bytes_le,
                                   uuid.UUID('1f5b84d1-9560-4345-9f49-74bc872143e4').bytes_le,
                                   DATA_FIRST, DATA_FIRST+data_sectors-1, 0, 'SCos data'.encode('utf-16le'))
    for current, backup, table in ((1, total_sectors-1, 2), (total_sectors-1, 1, total_sectors-33)):
        header = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0,
                                      current, backup, 34, total_sectors-34, disk_id, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', header, 16, zlib.crc32(header))
        image[current*512:current*512+92] = header
        image[table*512:table*512+len(entries)] = entries
    total = LAST-FIRST+1
    if total > 0x0ffffff5 + 2 * 130 + 32:
        raise ValueError('an ESP of %d sectors is wider than FAT32 addresses at one sector per cluster; a '
                         'stick up to 128 GiB works and a larger one wants a bigger cluster size, which this '
                         'packer does not implement rather than silently writing a volume the firmware '
                         'cannot read' % total)
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
    # Appended after the modules, and deliberately kept out of the driver index: that table counts things the
    # kernel could load as a driver, and a firmware image is not one of them.
    firmware = firmware_blobs(firmware_dir, sub=None) if firmware_dir else firmware_blobs(directory)
    v = provenance(directory, elf)
    # The record names the modules as the ESP holds them (FAT is upper-case), which is what a reader
    # checking the stick with another OS will see, so the two must not use different spellings.
    record_text, record_json = build_record(v, elf,
                                            [(stem + '.MOD', blob) for stem, blob in modules], firmware)
    (d / 'BUILD.TXT').write_text(record_text)
    (d / 'build.json').write_text(json.dumps(record_json, sort_keys=True, indent=1) + '\n')
    bc = allocate(record_text.encode())
    scos = (dot(0, dirs[3]) + entry('KERNEL.ELF', kc, len(elf)) + entry('KERNEL.CRC', cc, 4)
            + entry('BUILD.TXT', bc, len(record_text.encode())) + entry('DRVLIST.IDX', ic, len(index)))
    for stem, blob in modules:
        scos += entry(stem + '.MOD', allocate(blob), len(blob))
    for esp, blob in firmware:
        scos += entry(esp, allocate(blob), len(blob))
    # Every name this packer wrote, with the bytes it was written from.  The comparison below is the only thing
    # standing between a build and a stick that boots, and it compares the packed bytes with the staged ones
    # rather than the staged ones with themselves: a directory entry lying about its length, an ESP that ran out
    # of clusters, or a chain the FAT cut short all fail here instead of on a panel in front of somebody else.
    directory_data = [entry('EFI', dirs[1], directory=True)+entry('SCOS', dirs[3], directory=True),
                      dot(0, dirs[1])+entry('BOOT', dirs[2], directory=True),
                      dot(dirs[1], dirs[2])+entry('BOOTX64.EFI', ec, len(efi)),
                      scos]
    for c, data in zip(dirs, directory_data):
        offset = (data_sector+c-2)*512;image[offset:offset+len(data)] = data
    for i in range(2):
        offset = (FIRST+32+i*fat_sectors)*512;image[offset:offset+len(fat)] = fat
    readback = [('KERNEL', 'ELF', elf, ('SCOS',)), ('KERNEL', 'CRC', struct.pack('<I', zlib.crc32(elf)), ('SCOS',)),
                ('BUILD', 'TXT', record_text.encode(), ('SCOS',)), ('DRVLIST', 'IDX', index, ('SCOS',)),
                ('BOOTX64', 'EFI', efi, ('EFI', 'BOOT'))] \
               + [(stem, 'MOD', blob, ('SCOS',)) for stem, blob in modules] \
               + [(esp.rsplit('.', 1)[0], esp.rsplit('.', 1)[1], blob, ('SCOS',)) for esp, blob in firmware]
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=str(target.parent), prefix='.uefi-', delete=False) as f:
            temporary = Path(f.name)
            f.write(image)
            f.flush()
            os.fsync(f.fileno())
            reader = esp_reader()
            for stem, ext, blob, chain in readback:
                got = reader.read_named_file(temporary, stem, ext, chain=chain)
                if got is None:
                    raise ValueError('%s.%s was written but cannot be read back out of the packed ESP'
                                     % (stem, ext))
                if got != blob:
                    raise ValueError('%s.%s reads back as %d bytes (sha256 %s), not the %d staged: the directory '
                                     'entry and the cluster chain disagree, so the file on the disk is not the '
                                     'file this build has'
                                     % (stem, ext, len(got), hashlib.sha256(got).hexdigest()[:12], len(blob)))
        temporary.replace(target)
    finally:
        if temporary is not None:temporary.unlink(missing_ok=True)
    print(f'x64 UEFI GPT/ESP: {len(image)} bytes; loader {len(efi)}; ELF {len(elf)}; '
          f'coprocessor firmware {len(firmware)} file(s), every one read back out of the packed ESP')


def cli():
    '''`makedisk.py [directory] [--size BYTES] [--firmware DIR] [--out FILE]'.

    With no flags this packs build/ into dist/scos.img exactly as it always did, which is what the Makefile
    runs.  The other three exist for one job: writing a USB stick that has to carry a firmware image bigger
    than the 64 MiB volume the repository tracks, packed straight out of the driver directory already installed
    on the machine being flashed, with no build, no compiler and no filesystem surgery on the way.'''
    import argparse
    ap = argparse.ArgumentParser(prog='tools/makedisk.py')
    ap.add_argument('directory', nargs='?', default='build',
                    help='the staged build directory to pack (default: build)')
    ap.add_argument('--size', type=int, default=None, metavar='BYTES',
                    help='size the whole disk to BYTES, growing the ESP into the rest of it; a whole number of '
                         '512-byte sectors, at least the 64 MiB the tracked image uses')
    ap.add_argument('--firmware', default=None, metavar='DIR',
                    help='read the firmware roles from DIR instead of build/gsp - for example '
                         '/lib/firmware/nvidia/gb207/gsp, whose gsp-*.bin is the file no public archive ships')
    ap.add_argument('--out', default=None, metavar='FILE',
                    help='write the image to FILE instead of build/scos.img, which is the path the Makefile\'s dist target copies from')
    a = ap.parse_args()
    if a.size is not None and a.size % SECTOR:
        ap.error('--size has to be a multiple of %d bytes, because that is what a sector is' % SECTOR)
    main(a.directory, sectors=a.size, firmware_dir=a.firmware, out=a.out)


if __name__ == '__main__':
    cli()
