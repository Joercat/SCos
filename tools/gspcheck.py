#!/usr/bin/env python3
"""Check, and optionally install, the coprocessor firmware this OS hands to the FSP.

The panel on a machine with no firmware can only say `fw 0/5` and name the five files, and the one thing a
person standing in front of that machine has to get right before a flash is worth the cycle: the three blobs
the open driver loads for their chip, under the five names, with the sections this OS's parser insists on and
the widths *their* generation's FSP asks for.  Every one of those has been a wrong guess already - a 97-byte
public key refused by a kernel that expected Hopper's 384, a `.bin` that was really the wrong chip's, a file
truncated by a FAT write that reported no error - so the check happens here, on bytes, before the card is
asked to verify anything.

What it reads is the same contract the kernel reads, and tools/tests/test_gspcheck.py pins the two together:
the 52-byte FMC header nvkm/subdev/gsp/gh100.c memcmp's, the six section headers with their flags and their
CRC32 in sh_info, the sections named hash/publickey/signature/image, the `.fwimage` and `.fwsignature*` of
the resource-manager image, and the per-architecture widths of NVDM_PAYLOAD_COT's three blobs (nouveau's
`struct nvkm_fsp_func.cot', MIT: gh100 48/384/384 at version 1, gb100 and gb202 48/97/96 at version 2).

Usage:
    python3 tools/gspcheck.py /lib/firmware/nvidia/gb207/gsp
    python3 tools/gspcheck.py ~/fw --install /mnt/esp/SCOS         # copy the five names there and verify
    python3 tools/gspcheck.py ~/fw --install build/gsp && make dist # build the disk with them inside
Each role is judged by its own rules: the FMC image by this kernel's parser, the resource-manager image by the
section pair it needs, and the three files the FMC path loads beforehand by their container alone, because this
build stages them and does not yet run them.  With --install, every file that passes is written and read back;
one that fails is left out and named, which does not hold the others hostage.

Exit status is 0 only when the set that a handover needs is present and valid.
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import zlib

# The five names the boot stub looks for in \SCOS\ on the ESP, and what each one has to contain.  The upstream
# names come from NVKM_GSP_FIRMWARE_FMC/BOOTER in nvkm/subdev/gsp/priv.h: nvidia/<chip>/gsp/<role>-<ver>.bin.
ROLES = [
    ('GSP.BIN', 'gsp', 'resource-manager image', r'^gsp[-_.]'),
    ('GSPFMC.BIN', 'fmc', 'FMC image the FSP verifies and boots', r'^fmc[-_.]'),
    ('GSPBL.BIN', 'bootloader', 'falcon bootloader', r'^bootloader[-_.]'),
    ('GSPBTL.BIN', 'booter_load', 'booter load sequence', r'^booter_load[-_.]'),
    ('GSPBTU.BIN', 'booter_unload', 'booter unload sequence', r'^booter_unload[-_.]'),
]

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
FMC_SECTIONS = ('hash', 'publickey', 'signature', 'image')
FMC_SHT_PROGBITS, FMC_SHT_STRTAB = 1, 3
FMC_SHF_STRINGS, FMC_SHF_FLAGS = 0x20, 0xff00000a
IMAGE_MAX = 8 * 1024 * 1024                      # the most this kernel will copy for one image
# The reference driver's per-generation table, and the payload's field widths the blobs are copied into.
COT_TABLE = {0x18: (1, 48, 384, 384), 0x1a: (2, 48, 97, 96), 0x1b: (2, 48, 97, 96)}
FIELD_WIDTH = {'hash': 48, 'publickey': 384, 'signature': 384, 'image': IMAGE_MAX}
# What upstream calls each role's file, so a message can name the two files a swap would have mixed up.
ROLE_FILE = {'gsp': 'gsp-*.bin', 'fmc': 'fmc-*.bin', 'bootloader': 'bootloader-*.bin',
             'booter_load': 'booter_load-*.bin', 'booter_unload': 'booter_unload-*.bin'}


def le16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def le32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def le64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def sections(blob):
    """(is64, [(name, type, flags, offset, size, sh_info)]) straight out of the ELF section table, bounded by
    the file: a header that points outside the blob is reported as such rather than read through."""
    if len(blob) < 52 or blob[0:4] != b'\x7fELF':
        raise ValueError('not an ELF container (no \\x7fELF magic in the first four bytes)')
    if blob[5] != 1:
        raise ValueError('not little-endian (EI_DATA is %d)' % blob[5])
    is64 = blob[4] == 2
    if not is64 and blob[4] != 1:
        raise ValueError('neither ELFCLASS32 nor ELFCLASS64 (EI_CLASS is %d)' % blob[4])
    if is64:
        shoff, shentsize, shnum, strndx = le64(blob, 40), le16(blob, 58), le16(blob, 60), le16(blob, 62)
    else:
        shoff, shentsize, shnum, strndx = le32(blob, 32), le16(blob, 46), le16(blob, 48), le16(blob, 50)
    if shentsize == 0 or shnum == 0 or shoff + shnum * shentsize > len(blob):
        raise ValueError('the section table runs past the end of the file '
                         '(e_shoff %d, e_shnum %d, e_shentsize %d, file %d bytes)' % (shoff, shnum, shentsize,
                                                                                       len(blob)))
    table = shoff + strndx * shentsize
    if table + shentsize > len(blob):
        raise ValueError('the string table header is past the end of the file')
    if is64:
        so, ssz = le64(blob, table + 24), le64(blob, table + 32)
    else:
        so, ssz = le32(blob, table + 16), le32(blob, table + 20)
    if so + ssz > len(blob):
        raise ValueError('the section-name string table is past the end of the file')
    names = blob[so:so + ssz]

    out = []
    for i in range(shnum):
        h = shoff + i * shentsize
        if is64:
            name, typ, flags, off, size, info = (le32(blob, h), le32(blob, h + 4), le64(blob, h + 8),
                                                  le64(blob, h + 24), le64(blob, h + 32), le32(blob, h + 44))
        else:
            name, typ, flags, off, size, info = (le32(blob, h), le32(blob, h + 4), le32(blob, h + 8),
                                                  le32(blob, h + 16), le32(blob, h + 20), le32(blob, h + 28))
        end = names.find(b'\0', name) if name < len(names) else -1
        text = names[name:end].decode('ascii', 'replace') if end >= 0 else ''
        out.append((text, typ, flags, off, size, info))
    return is64, out


def check_fmc(blob, arch):
    """The FMC image, exactly as gh100_gsp_oneinit() reads it: the header must match the table byte for byte,
    the sections must be the shape the contract says, each one's CRC32 must be in sh_info, and the four named
    sections must be there at the widths this chip's FSP asks for."""
    problems, notes = [], []
    if len(blob) < 52 or blob[0:52] != FMC_HEADER:
        bad = next((i for i in range(min(len(blob), 52)) if blob[i] != FMC_HEADER[i]), None)
        problems.append('the first 52 bytes are not the FMC header the open driver compares against'
                        + ('' if bad is None else ' (they differ at byte %d: file 0x%02x, expected 0x%02x)'
                           % (bad, blob[bad], FMC_HEADER[bad])))
        return problems, notes, {}
    try:
        is64, secs = sections(blob)
    except ValueError as e:
        return ['FMC: %s' % e], notes, {}
    if is64:
        problems.append('the FMC image is ELFCLASS64; the FSP is given an ELFCLASS32 container (that is the '
                        'shape the .bin for the FMC path is built in, and the driver that boots this card '
                        'refuses anything else)')
    if len(secs) != 6:
        problems.append('the FMC container has %d sections and the contract has six (one string table, one '
                        'per named section, plus the null section)' % len(secs))
    for i, (name, typ, flags, off, size, info) in enumerate(secs):
        if i == 0:
            continue
        if i == 1:
            if typ != FMC_SHT_STRTAB or flags != FMC_SHF_STRINGS:
                problems.append('section 1 is the string table and should be SHT_STRTAB with SHF_STRINGS '
                                '(type %d, flags 0x%x)' % (typ, flags))
            continue
        if typ != FMC_SHT_PROGBITS or flags != FMC_SHF_FLAGS:
            problems.append('section %r is type %d flags 0x%x; every FMC section is SHT_PROGBITS with '
                            'SHF_MASKPROC|SHF_MASKOS|SHF_OS_NONCONFORMING|SHF_ALLOC (0x%x)'
                            % (name, typ, flags, FMC_SHF_FLAGS))
        if off + size > len(blob):
            problems.append('section %r claims %u bytes at offset %u, which runs past the %d-byte file'
                            % (name, size, off, len(blob)))
            continue
        if info and (zlib.crc32(blob[off:off + size]) ^ 0xffffffff) & 0xffffffff != info:
            problems.append('section %r carries CRC32 0x%08x in sh_info and its bytes give 0x%08x - the file '
                            'changed after it was signed, or it was truncated on the way here'
                            % (name, info, (zlib.crc32(blob[off:off + size]) ^ 0xffffffff) & 0xffffffff))
    found = {}
    for name, typ, flags, off, size, info in secs:
        if name in FMC_SECTIONS and name not in found:
            found[name] = (off, size)
    missing = [n for n in FMC_SECTIONS if n not in found]
    if missing:
        problems.append('the container has no %s section, and a handover payload has a field for each of '
                        '%s' % ('/'.join(missing), ', '.join(FMC_SECTIONS)))
        return problems, notes, found
    version, hash_w, pkey_w, sig_w = COT_TABLE[arch]
    for name, want in (('hash', hash_w), ('publickey', pkey_w), ('signature', sig_w)):
        got = found[name][1]
        if got != want:
            problems.append('the %s section is %u bytes and architecture 0x%x asks its FSP for %u - the '
                            'payload\'s field is %u bytes wide on every generation, so a wrong width here is '
                            'the wrong blob, not a padding question'
                            % (name, got, arch, want, FIELD_WIDTH[name]))
    image = found['image'][1]
    if not image:
        problems.append('the image section is empty')
    elif image > IMAGE_MAX:
        problems.append('the image section is %u bytes, past the %u MiB this kernel will copy into its own '
                        'pages for the FSP to read' % (image, IMAGE_MAX // (1024 * 1024)))
    elif image % 0x1000:
        notes.append('the image is %u bytes and will be padded to the next page (%u) when it is staged, which '
                     'is what the CRC the panel quotes is then taken over' % (image, (image + 0xfff) & ~0xfff))
    return problems, notes, found


def check_rm(blob):
    """The resource-manager image: ELF64, `.fwimage' as the payload and a `.fwsignature*' beside it, as
    r535_gsp_elf_section() looks for them.  It is not what the FSP is handed, and this says so rather than
    letting one file be mistaken for the other."""
    problems, notes = [], []
    try:
        is64, secs = sections(blob)
    except ValueError as e:
        return [str(e)], notes
    if not is64:
        problems.append('the resource-manager image is ELFCLASS32; the file that belongs under GSPFMC.BIN is '
                        'the one with the 52-byte FMC header, and the two are not interchangeable - the panel '
                        'says which of the two it found')
    names = [s[0] for s in secs]
    if '.fwimage' not in names:
        problems.append('no .fwimage section, which is the image this kernel would keep and point the '
                        'coprocessor at')
    sig = [n for n in names if n.startswith('.fwsignature')]
    if not sig:
        problems.append('no .fwsignature_* section; on this generation the open driver names it '
                        '.fwsignature_gb20x, and without it the image is unsigned as far as anything can tell')
    else:
        notes.append('signature section: %s' % sig[0])
    for name, typ, flags, off, size, info in secs:
        if name in ('.fwimage', sig[0] if sig else '') and off + size > len(blob):
            problems.append('section %r claims %u bytes at %u, past the end of the file' % (name, size, off))
    return problems, notes


def check_falcon(blob, role):
    """The three files the FMC path loads before the coprocessor is handed anything: `bootloader-*.bin' is a
    Falcon firmware ELF whose DMEM appif the reference driver patches so the falcon learns where the FRTS region
    is (nvkm/subdev/gsp/fwsec.c), and the two `booter_*' files are flat sequences for a chip that boots without
    the FSP.  What this kernel does with all three today is keep a copy and say what it is, so the rules here are
    only the rules this kernel would apply: the container parses, it is the class it has to be, and it fits in
    what the boot stub copies.  Anything stricter would be this tool inventing a requirement, and someone holding
    a correct file would be told it is wrong - which is the failure this whole checker exists to prevent."""
    problems, notes = [], []
    if blob[:4] != b'\x7fELF':
        if not blob:
            return ['the file is empty; the open driver reads a sequence of commands out of it'], notes
        if len(blob) > IMAGE_MAX:
            return ['%d bytes is past the %d MiB this kernel will copy for one image'
                    % (len(blob), IMAGE_MAX >> 20)], notes
        notes.append('flat sequence of %d bytes, kept and not run: the booter path belongs to a coprocessor that '
                     'boots without the FSP, which this chip does not' % len(blob))
        return problems, notes
    try:
        is64, secs = sections(blob)
    except ValueError as e:
        return [str(e)], notes
    if is64:
        problems.append('a Falcon firmware ELF is ELFCLASS32 and this file is ELFCLASS64 - that is the shape of '
                        'the resource-manager image, so gsp-*.bin and %s have been swapped' % ROLE_FILE[role])
    names = [s[0] for s in secs]
    for name, _typ, _flags, off, size, _info in secs:
        if off + size > len(blob):
            problems.append('section %r claims %u bytes at %u, past the end of the file' % (name, size, off))
    notes.append('Falcon ELF with %d section(s): %s - the FMC boot patches the DMEM appif inside this file '
                 'before it runs it, and this build does not patch it yet, so the file is staged, named and not '
                 'run' % (len(secs), ', '.join(n for n in names if n) or 'none named'))
    return problems, notes


def discover(src):
    """Map the files in `src' (a distro directory, a directory of ESP-named files, or one file) onto the five
    roles.  Naming is permissive; the contents are not."""
    by_role = {}
    files = []
    if os.path.isdir(src):
        for entry in sorted(os.listdir(src)):
            path = os.path.join(src, entry)
            if os.path.isfile(path) and entry.lower().endswith('.bin'):
                files.append((entry, path))
        if not files and len(os.listdir(src)) == 1:            # .../nvidia/gb207/gsp style: one level down
            for sub in sorted(os.listdir(src)):
                deep = os.path.join(src, sub)
                if not os.path.isdir(deep):
                    continue
                for entry in sorted(os.listdir(deep)):
                    if entry.lower().endswith('.bin'):
                        files.append((entry, os.path.join(deep, entry)))
    elif os.path.isfile(src):
        files.append((os.path.basename(src), src))
    for entry, path in files:
        low = entry.lower()
        for esp, role, _what, pattern in ROLES:
            if low == esp.lower():
                by_role.setdefault(role, path)
                break
            if re.match(pattern, low):
                by_role.setdefault(role, path)
                break
        else:
            if re.match(r'^gspbl\b|^gspfmc\b', low):
                continue
    return by_role, [e for e, _ in files]


def report(src, arch):
    out = {'source': src, 'arch': '0x%x' % arch, 'files': {}, 'problems': [], 'notes': []}
    by_role, seen = discover(src)
    out['found'] = sorted(os.path.basename(p) for p in by_role.values())
    out['considered'] = seen
    for esp, role, what, _pattern in ROLES:
        path = by_role.get(role)
        if not path:
            continue
        with open(path, 'rb') as handle:
            blob = handle.read()
        entry = {'esp_name': esp, 'file': os.path.basename(path), 'source': path, 'bytes': len(blob),
                 'crc32': '0x%08x' % ((zlib.crc32(blob) ^ 0xffffffff) & 0xffffffff), 'sections': {}}
        if role == 'fmc':
            problems, notes, found = check_fmc(blob, arch)
            for name, (off, size) in found.items():
                entry['sections'][name] = {'offset': off, 'bytes': size}
        elif role == 'gsp':
            problems, notes = check_rm(blob)
        else:
            problems, notes = check_falcon(blob, role)
        entry['ok'] = not problems
        out['files'][role] = entry
        out['problems'] += ['%s: %s' % (esp, p) for p in problems]
        out['notes'] += ['%s: %s' % (esp, n) for n in notes]
    missing = [esp for esp, role, _w, _p in ROLES if role not in by_role]
    out['missing'] = missing
    if 'fmc' not in by_role:
        out['problems'].insert(0, 'GSPFMC.BIN: nothing named fmc-* or GSPFMC.BIN was found in %s, and the '
                                  'handover is built from that file and no other' % src)
    if 'gsp' not in by_role:
        out['notes'].append('no gsp-*.bin: this OS stages no resource-manager image, so a handover that the '
                            'FSP accepts has no boot arguments to boot into (see the panel\'s sentence about '
                            'the declined fields)')
    # What the panel will say, so a paste can be compared with the check without guessing.
    out['panel'] = {'fw_staged': len(ROLES) - len(missing), 'fw_total': len(ROLES),
                    'handover_buildable': 'fmc' in by_role and out['files'].get('fmc', {}).get('ok', False)}
    return out


def install(src, dest, arch, force):
    """Copy the ESP-named files in, then read them back and compare - because the failure this is here to
    remove is a file that arrived shorter than it left, which only a round trip can see."""
    report_ = report(src, arch)
    os.makedirs(dest, exist_ok=True)
    results, skipped = [], []
    order = [r for _e, r, _w, _p in ROLES]
    for role, entry in sorted(report_['files'].items(), key=lambda kv: order.index(kv[0])):
        esp, = [e for e, r, _w, _p in ROLES if r == role]
        if not entry['ok'] and not force:
            # A refused file is left out on its own account, not everyone else's: the boot stub reads each of the
            # five independently and the kernel refuses each one by itself, so withholding a good FMC image
            # because a booter sequence is the wrong class would break the only path that could boot.
            skipped.append('%s: not written, because %s' % (esp, '; '.join(
                p.split(': ', 1)[1] if p.startswith(esp + ': ') else p for p in report_['problems']
                if p.startswith(esp + ': ')) or 'it failed its check'))
            continue
        source = entry['source']
        target = os.path.join(dest, esp)
        with open(source, 'rb') as handle:
            data = handle.read()
        with open(target, 'wb') as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        with open(target, 'rb') as handle:
            back = handle.read()
        results.append({'esp_name': esp, 'bytes': len(back),
                        'round_trip': 'identical' if back == data else 'DIFFERENT after the write (%d bytes '
                                                                      'read back, %d written)'
                                                                      % (len(back), len(data)),
                        'crc32': '0x%08x' % ((zlib.crc32(back) ^ 0xffffffff) & 0xffffffff)})
    if not results and not force:
        report_['problems'].append('nothing was installed: every file present here failed its own check.  Fix '
                                   'the file the message names, or pass --install-anyway to write them anyway and '
                                   'let the kernel be the judge')
    report_['skipped'] = skipped
    report_['installed'] = results
    report_['dest'] = dest
    return report_


def print_human(rep):
    print('gspcheck: %s, checked against architecture %s' % (rep['source'], rep['arch']))
    # One column per file name, as wide as the widest name there is: a fixed field would either pad the short
    # names into a wall of blanks or clip the long ones, and clipping is not on offer.
    width = max([len(e['file']) for e in rep['files'].values()] + [0]) + 1
    if rep.get('found'):
        print('  files: ' + ', '.join(rep['found']))
    for esp, role, what, _p in ROLES:
        entry = rep['files'].get(role)
        if not entry:
            print('  %-11s absent (%s)' % (esp, what))
            continue
        flag = 'ok' if entry['ok'] else 'REFUSED'
        extra = ''
        if role == 'fmc' and entry['sections']:
            extra = ' ' + ' '.join('%s=%d' % (n, s['bytes']) for n, s in sorted(entry['sections'].items()))
        print('  %-11s %-*s %8d B crc32 %s%s  [%s]' % (esp, width, entry['file'], entry['bytes'],
                                                         entry['crc32'], extra, flag))
    for line in rep.get('skipped', []):
        print('  skipped: ' + line)
    for line in rep['notes']:
        print('  note: ' + line)
    for line in rep['problems']:
        print('  PROBLEM: ' + line)
    panel = rep['panel']
    if not panel['fw_staged']:
        tail = 'and there is nothing on the disk to build a handover from'
    elif panel['handover_buildable']:
        tail = 'and build a handover from the FMC image'
    else:
        tail = ('and refuse to build one: the FMC image is the only file the handover is read from, and it is '
                'the one that failed')
    print('  the panel will say fw %d/%d %s' % (panel['fw_staged'], panel['fw_total'], tail))
    if rep.get('installed'):
        for entry in rep['installed']:
            print('  installed %-11s %8d B, read back %s, crc32 %s'
                  % (entry['esp_name'], entry['bytes'], entry['round_trip'], entry['crc32']))
        print('  into %s - if that is the ESP, the next boot sees them; if it is build/gsp, run make dist'
              % rep['dest'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('src', help='directory of firmware, or one file')
    ap.add_argument('--arch', default='0x1b', help='architecture whose FSP widths to check against '
                                                   '(0x1b Blackwell GB2xx, 0x1a GB10x, 0x18 Hopper)')
    ap.add_argument('--install', metavar='DIR', help='copy the ESP-named files into DIR and verify the write')
    ap.add_argument('--install-anyway', action='store_true', dest='force',
                    help='install even when a file is refused (the kernel will then be the one to say no)')
    ap.add_argument('--json', action='store_true', help='machine-readable output instead')
    args = ap.parse_args()
    arch = int(args.arch, 0)
    if arch not in COT_TABLE:
        raise SystemExit('gspcheck: architecture 0x%x has no FSP entry to check against; the reference driver '
                         'has rows for 0x18, 0x1a and 0x1b, and a chip outside those three has no mailbox at '
                         'these offsets at all, so there is nothing to install for it' % arch)
    rep = install(args.src, args.install, arch, args.force) if args.install else report(args.src, arch)
    if args.json:
        print(json.dumps(rep, indent=1, sort_keys=True))
    else:
        print_human(rep)
    return 0 if not rep['problems'] else 1


if __name__ == '__main__':
    sys.exit(main())
