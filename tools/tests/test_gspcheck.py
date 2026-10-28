#!/usr/bin/env python3
"""The firmware checker: its verdicts, and the fact that it checks the same contract the kernel checks.

`tools/gspcheck.py` is the part of this a person can use before a flash: it reads the blobs the open driver
loads for their chip, decides whether each is the shape this OS's parser insists on, and can put them on the
disk and read them back.  That only earns trust if its rules are the kernel's rules, so the first test compares
the two sources of constant by constant - the 52-byte FMC header, the four section names, the flags, the size
caps, the five ESP names, and the per-generation blob widths of the handover payload.  The rest hands the
checker files that are wrong in one specific way each and demands the sentence that names that way, because
"invalid" is nothing to act on and "the publickey section is 384 bytes and architecture 0x1b asks its FSP for
97" is.  The synthetic containers are built here rather than downloaded, because a real signed file cannot be
fetched from this sandbox; the shape they have is the one nvkm/subdev/gsp/gh100.c requires, which is what the
checker is written against.
"""

import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

import gspcheck                                                   # noqa: E402

NULL32 = bytes(40)


def fmc_blob(hash_bytes=48, pkey_bytes=97, sig_bytes=96, image_bytes=0x2000, names=None, header=None,
             corrupt=None, break_crc=False):
    """An FMC container in the shape gh100_gsp_oneinit() requires: the byte-exact 52-byte header, six section
    headers, the name table at index 1, every section SHT_PROGBITS with the process flags, each one's CRC32
    parked in sh_info, and nothing outside the file the headers point at."""
    names = ('hash', 'publickey', 'signature', 'image') if names is None else names
    sizes = {'hash': hash_bytes, 'publickey': pkey_bytes, 'signature': sig_bytes, 'image': image_bytes}
    strings = b'\0' + b''.join(n.encode() + b'\0' for n in names)
    shoff, shentsize, count = 52, 40, 6
    at = shoff + shentsize * count + len(strings)
    offsets = {}
    for name in names:
        offsets[name] = (at, sizes.get(name, 16))
        at += sizes.get(name, 16)

    blob = bytearray(header if header is not None else gspcheck.FMC_HEADER)
    blob += bytes(shentsize * count)
    blob += bytes(at - len(blob))
    for index, name in enumerate(names):
        off, size = offsets[name]
        blob[off:off + size] = bytes(((index + 1) * 0x11 + i) & 0xff for i in range(size))

    rows = bytearray(NULL32)                                   # section 0: the null header
    # sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize
    rows += struct.pack('<10I', 0, 3, 0x20, 0, shoff + shentsize * count, len(strings), 0, 0, 1, 0)
    for index, name in enumerate(names):
        off, size = offsets[name]
        crc = zlib.crc32(bytes(blob[off:off + size])) & 0xffffffff   # nouveau's formula, unmodified
        if break_crc:
            crc ^= 0xa5a5a5a5
        rows += struct.pack('<10I', 1 + sum(len(n) + 1 for n in names[:index]), 1, gspcheck.FMC_SHF_FLAGS, 0, off, size,
                            0, crc, 1, 0)
    blob[shoff:shoff + len(rows)] = rows
    blob[shoff + shentsize * count:shoff + shentsize * count + len(strings)] = strings
    if corrupt is not None:
        blob[corrupt] ^= 0x01                                       # after the CRCs, or nothing would be wrong
    return bytes(blob)


def rm_blob(elf64=True, image_section=True, signature='.fwsignature_gb20x'):
    """A resource-manager image: ELF64 with `.fwimage' and a `.fwsignature_*' beside it, as
    r535_gsp_elf_section() looks for them."""
    names = [n for n in ('.shstrtab', '.fwimage', signature) if n]
    if not image_section:
        names.remove('.fwimage')
    strings = b'\0' + b''.join(n.encode() + b'\0' for n in names)
    shentsize = 64 if elf64 else 40
    shoff = (64 if elf64 else 52)
    count = len(names)          # the first row is the string table itself; there is no null row here
    at = shoff + shentsize * count + len(strings)
    at = (at + 15) & ~15
    offsets = {}
    for index, name in enumerate(names):
        offsets[name] = at
        at += 0x200
    blob = bytearray(b'\x7fELF' + bytes([2 if elf64 else 1, 1, 1, 0]) + bytes(8))
    blob += bytes(at - len(blob))
    for index, name in enumerate(names):
        off = offsets[name]
        blob[off:off + 0x200] = bytes(((index + 2) * 0x21 + i) & 0xff for i in range(0x200))
    rows = b''
    for index, name in enumerate(names):
        if name == '.shstrtab':
            typ, flags, off, size, info = 3, 0x20, shoff + shentsize * count, len(strings), 0
        else:
            typ, flags, off, size, info = 1, 0x40000002, offsets[name], 0x200, 0
        name_off = 1 + sum(len(n) + 1 for n in names[:index])
        if elf64:
            rows += struct.pack('<2I2Q2Q2I2Q', name_off, typ, flags, 0, off, size, 0, info, 1, 0)
        else:
            rows += struct.pack('<10I', name_off, typ, flags, 0, off, size, 0, info, 1, 0)
    blob[shoff:shoff + len(rows)] = rows[:shentsize * count]
    blob[shoff + shentsize * count:shoff + shentsize * count + len(strings)] = strings
    if elf64:
        struct.pack_into('<HHIQQQIHHHHHH', blob, 16, 1, 62, 1, 0, 0, shoff, 0, 64, 0, 0, shentsize, count,
                         0)                                       # e_shstrndx: .shstrtab is section 0
    else:
        struct.pack_into('<HHIIIIIHHHHHH', blob, 16, 1, 3, 1, 0, 0, shoff, 0, 52, 0, 0, shentsize, count, 0)
    return bytes(blob)


def run(*args):
    return subprocess.run([sys.executable, str(ROOT / 'tools' / 'gspcheck.py')] + [str(a) for a in args],
                          cwd=ROOT, capture_output=True, text=True)


def tool_report(path, arch='0x1b'):
    result = run(path, '--arch', arch, '--json')
    assert result.returncode in (0, 1), result.stdout + result.stderr
    assert not result.stderr.strip(), result.stderr
    return json.loads(result.stdout), result.returncode


def write(dirpath, name, blob):
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, name), 'wb') as handle:
        handle.write(blob)
    return dirpath


def test_the_checker_and_the_kernel_read_the_same_contract():
    kernel = (ROOT / 'kernel/drivers/gpu/gpu_firmware.c').read_text()
    fsp = (ROOT / 'kernel/drivers/gpu/gpu_fsp.c').read_text()
    boot = (ROOT / 'kernel/include/boot.h').read_text()

    m = re.search(r'static const uint8_t fmc_header\[52\] = \{(.*?)\n\};', kernel, re.S)
    assert m, 'the kernel no longer carries the FMC header table, so nothing here pins the two together'
    listed = re.findall(r"0x([0-9a-fA-F]{1,2})|'([^']*)'|(\d+)", m.group(1))
    kernel_bytes = bytes(int(a, 16) if a else (ord(b) if b else int(c)) for a, b, c in listed)
    assert len(kernel_bytes) == 52, 'the table in the kernel is %d bytes, not 52' % len(kernel_bytes)
    assert kernel_bytes == gspcheck.FMC_HEADER, \
        'the kernel compares %s and the checker compares %s - one rule kept in two places, disagreeing' % (
            kernel_bytes.hex(), gspcheck.FMC_HEADER.hex())
    # The section flags, in all three places the rule is written down.  This number was wrong in the kernel
    # until a real fmc-570.144.bin from a 615.71.10 install said 0xfff00102 for every one of its sections,
    # which is 0xff00000a plus SHF_MASKPROC: the mask's value had been left out of the sum, so the genuine file
    # was refused.  A rule kept in three files needs all three checked, or the fix only lands in one of them.
    for where in ('kernel/drivers/gpu/gpu_firmware.c', 'tools/tests/gpu_fsp_host.c',
                  'tools/tests/gsp_fw_kernel_host.c'):
        text = (ROOT / where).read_text()
        assert '0xfff00102' in text, '%s still carries a section-flags value of its own' % where
    kernel = (ROOT / 'kernel/drivers/gpu/gpu_firmware.c').read_text()
    assert re.search(r'#define\s+FMC_SHF_FLAGS\s+0xfff00102u', kernel), \
        'the kernel no longer defines the flag mask the real file carries'
    assert '0xff00000a' not in fsp, 'the wrong mask survived in a place that has no comment to explain it'
    assert gspcheck.FMC_SHF_FLAGS == 0xfff00102

    for name in gspcheck.FMC_SECTIONS:
        assert '"%s"' % name in kernel, \
            'the kernel no longer looks for the %r section the checker insists on' % name
    assert re.search(r'#define FSP_IMAGE_MAX\s+\(8u \* 1024u \* 1024u\)', fsp), \
        "the kernel's image cap is no longer 8 MiB and the checker quotes that number as its own"
    assert gspcheck.IMAGE_MAX == 8 * 1024 * 1024
    flags = int(re.search(r'#define FMC_SHF_FLAGS\s+(0x[0-9a-f]+)u', kernel).group(1), 16)
    assert flags == gspcheck.FMC_SHF_FLAGS, 'section flags: kernel 0x%x, checker 0x%x' % (
        flags, gspcheck.FMC_SHF_FLAGS)
    # The header table pins the layout the checker walks, so its own count rule is a safety net rather than a
    # test any file can reach: e_shoff 52, e_shentsize 40, six sections, string table at index 1.
    assert struct.unpack_from('<I', gspcheck.FMC_HEADER, 32)[0] == 52
    assert struct.unpack_from('<H', gspcheck.FMC_HEADER, 46)[0] == 40
    assert struct.unpack_from('<H', gspcheck.FMC_HEADER, 48)[0] == 6
    assert struct.unpack_from('<H', gspcheck.FMC_HEADER, 50)[0] == 1
    assert gspcheck.FMC_HEADER[4:5] == b'\x01', 'the FMC container is not ELFCLASS32 in the table any more'
    stub_names = re.findall(r'"([A-Z0-9]+\.BIN)"', boot)
    checker_names = [e for e, _r, _w, _p in gspcheck.ROLES]
    assert stub_names == checker_names, \
        'the boot stub looks for %s and the checker installs %s' % (stub_names, checker_names)

    body = fsp[fsp.index('static void cot_parameters(void)'):]
    body = body[:body.index('\n}\n')]
    for arch, (version, hash_w, pkey_w, sig_w) in gspcheck.COT_TABLE.items():
        clause = re.search(r'case 0x%xu:.*?break;' % arch, body, re.S | re.I)
        assert clause, 'architecture 0x%x has no row in cot_parameters() but the checker claims one' % arch
        text = clause.group(0)
        assert 'cot_version = %uu' % version in text, \
            '0x%x: the container version the checker expects (%d) is not what the kernel sets' % (arch, version)
        assert 'cot_pkey_bytes = %uu' % pkey_w in text and 'cot_sig_bytes = %uu' % sig_w in text, \
            '0x%x: the checker says %d/%d and the kernel says otherwise' % (arch, pkey_w, sig_w)
    assert 'fsp_state.cot_hash_bytes = %uu;' % hash_w in body, 'the hash width is no longer shared by the rows'


def test_a_file_of_the_right_shape_passes_and_says_what_the_panel_will_say():
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, 'fmc-570.144.bin', fmc_blob())
        rep, status = tool_report(tmp)
        assert status == 0, rep['problems']
        entry = rep['files']['fmc']
        assert entry['ok'] and entry['bytes'] == len(fmc_blob())
        assert entry['sections']['publickey']['bytes'] == 97 and entry['sections']['signature']['bytes'] == 96
        assert entry['crc32'] == '0x%08x' % (zlib.crc32(fmc_blob()) & 0xffffffff), \
            'the file-level checksum the report shows is not the one the build record and the panel quote'
        assert rep['panel'] == {'fw_staged': 1, 'fw_total': 5, 'handover_buildable': True}, rep['panel']
        assert rep['missing'] == ['GSP.BIN', 'GSPBL.BIN', 'GSPBTL.BIN', 'GSPBTU.BIN'], rep['missing']
        assert any('no boot arguments to boot into' in n for n in rep['notes']), rep['notes']
        # ...and that a file whose sections *do* carry the reference driver's checksum is accepted without being
        # complained about: the fixture above writes sh_info as crc32_le(~0, bytes) ^ ~0, which is nouveau's
        # formula, and the rule this tool used to apply was that value inverted - a bug that reported four correct
        # CRCs as absent ones and made this tool and the kernel disagree about one file.
        assert not [n for n in rep['notes'] if 'not a checksum' in n], rep['notes']


def test_the_wrong_generation_is_a_different_sentence_from_a_broken_file():
    with tempfile.TemporaryDirectory() as tmp:
        hopper = write(os.path.join(tmp, 'hopper'), 'fmc-570.144.bin',
                       fmc_blob(pkey_bytes=384, sig_bytes=384))
        rep, status = tool_report(hopper, '0x18')
        assert status == 0, rep['problems']
        rep, status = tool_report(hopper, '0x1b')
        assert status == 1, rep
        assert any('publickey section is 384 bytes and architecture 0x1b asks its FSP for 97' in p
                   for p in rep['problems']), rep['problems']
        assert any('signature section is 384 bytes' in p for p in rep['problems']), rep['problems']
        black = write(os.path.join(tmp, 'black'), 'fmc-570.144.bin', fmc_blob())
        rep, status = tool_report(black, '0x18')
        assert status == 1 and any('publickey section is 97 bytes and architecture 0x18 asks its FSP for 384'
                                   in p for p in rep['problems']), rep['problems']
        # And the architecture itself has to be one of the rows, in the tool as in the driver.
        result = run(black, '--arch', '0x19')
        assert result.returncode != 0 and 'has no FSP entry to check against' in result.stdout + result.stderr, \
            result.stdout + result.stderr


def test_each_way_a_file_can_be_wrong_gets_its_own_words():
    good = fmc_blob()
    with tempfile.TemporaryDirectory() as tmp:
        cases = [
            ('header', 'fmc-1.bin', fmc_blob(header=bytes(52)), 'not the FMC header'),
            # A sh_info that is not the section's CRC32 is said, not obeyed - the genuine fmc-570.144.bin
            # measured on a 615.71.10 install fails that rule on all four sections while its bytes match the
            # driver's own arrays, so refusing here would hide the card's answer behind a header field.
            ('signed-bytes', 'fmc-2.bin', fmc_blob(corrupt=-1), None),
            ('crc-field', 'fmc-3.bin', fmc_blob(break_crc=True), None),
            ('too-big', 'fmc-4.bin', fmc_blob(image_bytes=gspcheck.IMAGE_MAX + 0x1000),
             'past the 8 MiB this kernel will copy'),
            ('empty-image', 'fmc-6.bin', fmc_blob(image_bytes=0), 'the image section is empty'),
        ]
        for name, filename, blob, needle in cases:
            one = write(os.path.join(tmp, name), filename, blob)
            rep, status = tool_report(one)
            if needle is None:
                assert status == 0, (name, rep['problems'])
                assert any('so the field is not a checksum in this file' in n for n in rep['notes']), \
                    (name, rep['notes'])
                assert rep['panel']['handover_buildable'] is True, \
                    'a file whose sections still carry a signature was vetoed by a bookkeeping field'
                continue
            assert status == 1, (name, rep)
            assert any(needle in p for p in rep['problems']), (name, rep['problems'])
        # A section name gone missing is the container's own problem, not a truncation.
        one = write(os.path.join(tmp, 'missing'), 'fmc-1.bin', fmc_blob(names=('hash', 'publickey', 'image')))
        rep, status = tool_report(one)
        assert any('has no signature section' in p for p in rep['problems']), rep['problems']
        # Truncation has to be caught as an out-of-range section rather than a wild read.
        one = write(os.path.join(tmp, 'cut'), 'fmc-1.bin', good[:len(good) - 0x100])
        rep, status = tool_report(one)
        assert status == 1 and any('runs past the' in p or 'past the end of the file' in p or
                                  'changed after it was signed' in p for p in rep['problems']), rep['problems']
        # A file that is not an ELF at all, and one that is neither class.
        one = write(os.path.join(tmp, 'junk'), 'fmc-1.bin', b'NVFirmware\x00' + bytes(64))
        rep, status = tool_report(one)
        assert status == 1 and any('not the FMC header' in p for p in rep['problems']), rep['problems']
        one = write(os.path.join(tmp, 'class'), 'fmc-1.bin', fmc_blob(header=bytes(gspcheck.FMC_HEADER[:4] +
                                                                                   b'\x03\x01\x01\x00' +
                                                                                   gspcheck.FMC_HEADER[8:52])))
        rep, status = tool_report(one)
        assert any('not the FMC header' in p for p in rep['problems']), rep['problems']


def test_the_resource_manager_image_is_checked_as_what_it_is():
    with tempfile.TemporaryDirectory() as tmp:
        # The FMC file is present in every fixture here: without it the checker has a bigger complaint than the
        # one under test, and the point of these cases is that each file is judged by its own rules.
        def fixture(name, blob):
            path = os.path.join(tmp, name)
            write(path, 'fmc-570.144.bin', fmc_blob())
            write(path, 'gsp-570.144.bin', blob)
            return path

        good = fixture('ok', rm_blob())
        rep, status = tool_report(good)
        assert status == 0, rep['problems']
        assert any('signature section: .fwsignature_gb20x' in n for n in rep['notes']), rep['notes']
        assert not any('no boot arguments' in n for n in rep['notes']), rep['notes']
        noimage = fixture('noimage', rm_blob(image_section=False))
        rep, _ = tool_report(noimage)
        assert any('no .fwimage section' in p for p in rep['problems']), rep['problems']
        nosig = fixture('nosig', rm_blob(signature=None))
        rep, _ = tool_report(nosig)
        assert any('no .fwsignature_* section' in p for p in rep['problems']), rep['problems']
        # The two files are not interchangeable, and swapping them is said as the swap.
        swap = fixture('swap', fmc_blob())
        rep, _ = tool_report(swap)
        assert any('resource-manager image is ELFCLASS32' in p for p in rep['problems']), rep['problems']
        # The FMC file in the fmc slot is still itself, so only the slot holding an FMC container complains -
        # which is what makes the sentence worth having: it says where that file belongs.
        assert rep['files']['fmc']['ok'] is True and rep['files']['gsp']['ok'] is False, rep['files']
        assert any('GSPFMC.BIN' in p for p in rep['problems'] if 'resource-manager' in p), rep['problems']
        missing = write(os.path.join(tmp, 'nofmc'), 'gsp-570.144.bin', rm_blob())
        rep, _ = tool_report(missing)
        assert any('nothing named fmc-* or GSPFMC.BIN was found' in p for p in rep['problems']), rep['problems']
        assert rep['panel'] == {'fw_staged': 1, 'fw_total': 5, 'handover_buildable': False}, rep['panel']


def test_installing_proves_the_bytes_arrived_under_the_names_the_stub_reads():
    with tempfile.TemporaryDirectory() as tmp:
        src = write(tmp, 'fmc-570.144.bin', fmc_blob())
        write(src, 'gsp-570.144.bin', rm_blob())
        dest = os.path.join(tmp, 'esp')
        result = run(src, '--install', dest, '--json')
        assert result.returncode == 0, result.stdout + result.stderr
        rep = json.loads(result.stdout)
        installed = {e['esp_name']: e for e in rep['installed']}
        assert sorted(installed) == ['GSP.BIN', 'GSPFMC.BIN'], sorted(installed)
        for entry in installed.values():
            assert entry['round_trip'] == 'identical', entry
        rep2, status = tool_report(dest)
        assert status == 0, rep2['problems']
        assert sorted(rep2['files']) == ['fmc', 'gsp'], rep2['files'].keys()
        assert rep2['found'] == ['GSP.BIN', 'GSPFMC.BIN'], rep2['found']
        # One refused file is that file's problem: it stays off the disk, is named, and the good files still go.
        partial = os.path.join(tmp, 'partial')
        write(partial, 'fmc-570.144.bin', fmc_blob())
        write(partial, 'gsp-570.144.bin', b'not an elf' + bytes(64))
        result = run(partial, '--install', os.path.join(tmp, 'esp2'))
        assert result.returncode == 1, result.stdout + result.stderr
        assert 'GSP.BIN: not written' in result.stdout, result.stdout
        assert os.path.exists(os.path.join(tmp, 'esp2', 'GSPFMC.BIN')), \
            'a good FMC image was withheld because an unrelated file was bad - that is how a stick stops booting'
        assert not os.path.exists(os.path.join(tmp, 'esp2', 'GSP.BIN')), 'a refused file went onto the disk'
        # Nothing passes, nothing is written, and the reason says so.
        bad = write(os.path.join(tmp, 'bad'), 'fmc-570.144.bin', b'not an elf' + bytes(64))
        result = run(bad, '--install', os.path.join(tmp, 'esp2b'))
        assert result.returncode == 1, result.stdout
        assert 'nothing was installed' in result.stdout, result.stdout
        assert not os.path.exists(os.path.join(tmp, 'esp2b', 'GSPFMC.BIN')), \
            'a file the checker refused still went onto the disk'
        # ...and --install-anyway means what it says, because the kernel is the stricter judge: the file
        # goes in, the status still reports the refusal, and the words say that it was put in anyway.
        result = run(bad, '--install', os.path.join(tmp, 'esp3'), '--install-anyway')
        assert os.path.exists(os.path.join(tmp, 'esp3', 'GSPFMC.BIN')), result.stdout + result.stderr
        assert result.returncode == 1, 'installing a refused file must not read as a clean check'
        assert 'not the FMC header' in result.stdout, result.stdout


def test_the_five_files_the_open_driver_loads_are_five_files_of_five_different_shapes():
    """The regression this exists for: `bootloader-*.bin' is a Falcon ELF32 and the two booter files are flat
    sequences, so judging them by the resource-manager image's rules would refuse a machine whose firmware is
    perfect - and then refuse to install the good files beside them."""
    with tempfile.TemporaryDirectory() as tmp:
        src = write(tmp, 'fmc-570.144.bin', fmc_blob())
        write(src, 'gsp-570.144.bin', rm_blob())
        write(src, 'bootloader-570.144.bin', fmc_blob(image_bytes=0x1000))
        write(src, 'booter_load-570.144.bin', b'\x00' * 256)
        write(src, 'booter_unload-570.144.bin', b'\x00' * 256)
        rep, status = tool_report(src)
        assert status == 0, rep['problems']
        assert rep['panel'] == {'fw_staged': 5, 'fw_total': 5, 'handover_buildable': True}, rep['panel']
        assert len(rep['missing']) == 0, rep['missing']
        assert sum(1 for n in rep['notes'] if 'Falcon ELF' in n) == 1, rep['notes']
        assert sum(1 for n in rep['notes'] if 'flat sequence' in n) == 2, rep['notes']
        result = run(src, '--install', os.path.join(tmp, 'esp'), '--json')
        assert result.returncode == 0, result.stdout + result.stderr
        names = [e['esp_name'] for e in json.loads(result.stdout)['installed']]
        assert names == ['GSP.BIN', 'GSPFMC.BIN', 'GSPBL.BIN', 'GSPBTL.BIN', 'GSPBTU.BIN'], names
        assert all(e['round_trip'] == 'identical' for e in json.loads(result.stdout)['installed'])
        # The swap a person can actually make, in the slot where it is not the FMC file's rules.
        swap = write(os.path.join(tmp, 'swap'), 'fmc-570.144.bin', fmc_blob())
        write(swap, 'gsp-570.144.bin', rm_blob())
        write(swap, 'bootloader-570.144.bin', rm_blob())          # ELF64: the resource-manager shape
        rep, status = tool_report(swap)
        assert status == 1 and any('gsp-*.bin and bootloader-*.bin have been swapped' in p for p in rep['problems']), \
            rep['problems']
        empty = write(os.path.join(tmp, 'empty'), 'fmc-570.144.bin', fmc_blob())
        write(empty, 'booter_load-570.144.bin', b'')
        rep, status = tool_report(empty)
        assert status == 1 and any('the file is empty' in p for p in rep['problems']), rep['problems']
        text = run(empty).stdout
        assert 'and build a handover from the FMC image' in text, text
        assert 'GSPBTL.BIN  booter_load-570.144.bin' in text, text


def test_the_paste_a_person_sends_carries_the_verdict():
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, 'fmc-570.144.bin', fmc_blob(hash_bytes=32))
        result = run(tmp)
        assert result.returncode == 1, result.stdout
        text = result.stdout
        assert 'GSPFMC.BIN' in text and 'hash=32' in text, text
        assert 'the hash section is 32 bytes and architecture 0x1b asks its FSP for 48' in text, text
        assert 'the panel will say fw 1/5 and refuse to build one' in text, text
        assert 'the FMC image is the only file the handover is read from' in text, text
        assert 'REFUSED' in text, text


def real_shaped_fmc():
    """A file with the measurements of `fmc-570.144.bin' as a 615.71.10 install carries it: four sections of 48,
    199240, 97 and 96 bytes, flags 0xfff00102, and `sh_info' values that are not the CRC32 of the bytes they
    describe (0x5f963b4f / 0x7ed85d13 / 0x704dae4b / 0x877bdef9 measured in that file, against 0xa069c4b0 /
    0x8127a2ec / 0x8fb251b4 / 0x78842106 computed over those same lengths).  That the key's own CRC is
    0x8fb251b4 - the CRC of the public key extracted from the driver's GB202 arrays - is what shows the bytes are
    read correctly and the field is packaging rather than integrity.  The fixture exists so that accepting a
    genuine file is a tested property, and so that nobody reinstates the stricter rule without noticing that it
    rejects the real thing.  It is built from this suite's own container, because that part is already known
    good, and then has the four measured `sh_info' values and the measured length written over it."""
    blob = bytearray(fmc_blob(image_bytes=199240))
    measured = {'hash': 0x5f963b4f, 'publickey': 0x704dae4b, 'signature': 0x877bdef9, 'image': 0x7ed85d13}
    for index, name in enumerate(('hash', 'publickey', 'signature', 'image'), start=2):
        struct.pack_into('<I', blob, 52 + 40 * index + 28, measured[name])    # sh_info, 28 in an ELF32 row
    blob += bytearray(199820 - len(blob))          # the length the real file reports, padding and all
    return bytes(blob)


def test_a_file_shaped_like_the_vendor_is_accepted_with_the_difference_spoken():
    """The whole point of the two constants: a genuine file comes through usable, and the one rule the
    reference driver adds on top is reported rather than obeyed."""
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, 'fmc-570.144.bin', real_shaped_fmc())
        rep, status = tool_report(tmp)
        assert status == 0, rep['problems']
        entry = rep['files']['fmc']
        assert entry['ok'] and entry['bytes'] == 199820, rep['problems']
        assert {n: s['bytes'] for n, s in entry['sections'].items()} == {'hash': 48, 'image': 199240,
                                                                        'publickey': 97,
                                                                        'signature': 96}, entry['sections']
        said = [n for n in rep['notes'] if 'so the field is not a checksum in this file' in n]
        assert len(said) == 4, 'all four sections trip the CRC rule in a real file, so all four are said: %r' % rep['notes']
        assert rep['panel']['handover_buildable'] is True, rep['panel']
        assert (rep['panel']['fw_staged'], rep['panel']['fw_total']) == (1, 5), rep['panel']
        # The flags this fixture carries are the flags the kernel asks for, so the two cannot drift apart again.
        assert gspcheck.FMC_SHF_FLAGS == 0xfff00102, 'a real fmc-*.bin reports 0xfff00102 on every section'


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for name, fn in tests:
        fn()
    print('PASS: gspcheck - the kernel\'s contract held in one place, every refusal named by what it is, and '
          'an install that proves the bytes arrived')


if __name__ == '__main__':
    main()
