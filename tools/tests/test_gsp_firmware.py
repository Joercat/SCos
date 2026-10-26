"""The coprocessor-firmware intake: the examiner that runs inside the boot stub, the names it reads, the
record it hands over, and the paragraph the panel prints from that record on a booted machine.

Why this suite exists: SCos now reads five files off the boot volume that it has no code to use yet, so the
whole value of the feature is that it tells the truth about a disk - and the input is a file somebody
copied, parsed by code that runs before any kernel exists to catch a bad pointer.  Both halves are measured
here.  The C half compiles `boot/uefi/gsp_fw.c' exactly as the stub compiles it and drives it with containers
built field by field, under ASan and UBSan: a truncated section table, a string table that does not
terminate inside the image, a header that lies about how many entries follow it.  The python half checks the
three places the five role names are written - kernel/include/boot.h, tools/makedisk.py and the panel text -
are still the same five names, and that the intake refuses what it cannot carry instead of shipping it.

The boot half then runs the real stub against a real FAT32 volume holding a real container, because a
fixture cannot show that the read, the check and the handover survive the filesystem, the handoff and the
2560-byte panel buffer at the same time.

What none of this claims: that a real 570.144 blob was staged and booted.  The files are not distributed
here.  Whether an image is then handed to the card is the FSP's answer to a query, which is what the
panel's own paragraph reports, and this file reads that paragraph off a booted machine.
"""
import importlib.util
import os
import re
import zlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Read from the same header the two consumers compile against, so this file cannot test a buffer size that
# no shipped code path uses.
REPORT_CAP = int(re.search(r'#define SCOS_GRAPHICS_REPORT (\d+)',
                           (ROOT / 'kernel/include/scos.h').read_text()).group(1))

HOST_CFLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CHECKS = 122                      # the boot stub's examiner, pinned: a dropped case is a silent loosening
KERNEL_CHECKS = 25                # the kernel's own reading of the bytes, same reason


def run(cmd, **kwargs):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kwargs)
    assert result.returncode == 0, ' '.join(str(x) for x in cmd) + '\n' + result.stdout + result.stderr
    assert not result.stderr.strip(), ' '.join(str(x) for x in cmd) + '\n' + result.stderr
    return result


def build_container(payload_bytes=1024, signature=True, sections=None, shentsize=64, shnum=None,
                    shstrndx=None, shoff=None):
    """An ELF64 image with `.fwimage' and (by default) `.fwsignature_gb20x', laid out the way the loader
    this mirrors expects: payload data first, section headers aligned after it, names in a string table at
    the end.  Every knob exists so exactly one field can be made to lie while the rest stay sound."""
    strings = b'\0.fwimage\0.fwsignature_gb20x\0.shstrtab'

    def off(name):
        # The offset of a name inside the table, counted from the table start - not the offset of the NUL
        # that precedes it, which is what a substring search returns and what cost a full minute of "no
        # .fwimage section" on a booted machine before the cross-check below was written.
        return strings.index(b'\0' + name.encode()) + 1

    if sections is None:
        sections = [('null', 0), ('.fwimage', payload_bytes)]
        if signature:
            sections.append(('.fwsignature_gb20x', 512))
        sections.append(('.shstrtab', len(strings)))
    image = bytearray(64)
    at = 64
    data = {}
    for name, size in sections:
        if name == 'null':
            blob = b''
        elif name == '.shstrtab':
            blob = strings
        else:
            blob = bytes((i * 7) & 0xff for i in range(size))
        data[name] = (at, size, blob)
        at += size
    at = (at + 7) & ~7
    table = []
    for index, (name, _size) in enumerate(sections):
        offset, size, _blob = data[name]
        sh_name = 0 if index == 0 else off(name)
        table.append(struct.pack('<IIQQQQIIQQ', sh_name, 0 if index == 0 else 1, 0, 0, offset, size,
                                0, 0, 1, 0))
    headers = b''.join(table)
    image.extend(bytearray(at + len(headers) - len(image)))
    image[0:4] = b'\x7fELF'
    image[4], image[5], image[6] = 2, 1, 1
    struct.pack_into('<Q', image, 40, shoff if shoff is not None else at)
    # e_ehsize 52, e_phentsize 54, e_phnum 56, e_shentsize 58, e_shnum 60, e_shstrndx 62: the header is
    # dense, and each field is written at its own offset rather than in one run, because the ones between
    # these are program-header fields this container does not have.
    struct.pack_into('<HHH', image, 52, 64, 56, 0)
    struct.pack_into('<HH', image, 58, shentsize, shnum if shnum is not None else len(sections))
    struct.pack_into('<H', image, 62, shstrndx if shstrndx is not None else len(sections) - 1)
    for name, _size in sections:
        offset, size, blob = data[name]
        image[offset:offset + size] = blob
    image[at:at + len(headers)] = headers
    return bytes(image)


def fmc_template_from_kernel():
    """The 52 bytes the kernel's parser compares an FMC image against, read out of the C source they are
    written in.  Copying them into this file would let the two drift and the test agree with itself, which is
    the failure this whole suite exists to avoid."""
    text = (ROOT / 'kernel' / 'drivers' / 'gpu' / 'gpu_firmware.c').read_text()
    start = text.index('fmc_header[52] = {')
    body = text[start + len('fmc_header[52] = {'):text.index('};', start)]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    out = []
    for token in body.split(','):
        token = token.strip()
        if not token:
            continue
        out.append(ord(token[1]) if token.startswith("'") else int(token, 0))
    assert len(out) == 52, 'the template in the kernel source is not 52 bytes: %d' % len(out)
    return bytes(out)


def build_fmc_image(hash_bytes=32, publickey_bytes=4, signature_bytes=256, image_bytes=512, with_crc=True,
                    section_flags=0xff00000a, strtab_type=3, image_at=None):
    """An FMC image laid out the way nvkm/subdev/gsp/gh100.c requires one: the published header verbatim, six
    40-byte section headers, then the string table and the four sections it names, each carrying in sh_info the
    standard CRC32 of its own bytes.  One knob per rule, so a failure says which rule caught it."""
    header = fmc_template_from_kernel()
    assert struct.unpack_from('<HHHHH', header, 40)[0:0] == () and len(header) == 52
    names = b'\0hash\0publickey\0signature\0image\0'
    strings_at = 52 + 6 * 40
    order = [('hash', hash_bytes), ('publickey', publickey_bytes), ('signature', signature_bytes),
             ('image', image_bytes)]
    at = strings_at + len(names)
    where = {}
    blobs = []
    for name, size in order:
        offset = image_at if (name == 'image' and image_at is not None) else at
        blobs.append((name, offset, size, bytes((i * 11) & 0xff for i in range(size))))
        if name == 'image' and image_at is not None:
            where[name] = (offset, size)
            continue
        where[name] = (offset, size)
        at = offset + size
    total = max(offset + size for _n, offset, size, _b in blobs)

    def shdr(name_off=0, type_=0, flags=0, offset=0, size=0, info=0):
        return struct.pack('<IIIIIIIIII', name_off, type_, flags, 0, offset, size, 0, info, 0, 0)

    table = [shdr(), shdr(1, strtab_type, 0x20, strings_at, len(names))]
    for name, offset, size, blob in blobs:
        table.append(shdr(names.index(b'\0' + name.encode()) + 1, 1, section_flags, offset, size,
                          zlib.crc32(blob) if with_crc else 0))
    image = bytearray(total)
    image[0:52] = header
    image[52:52 + 6 * 40] = b''.join(table)
    image[strings_at:strings_at + len(names)] = names
    for _name, offset, size, blob in blobs:
        image[offset:offset + size] = blob
    return bytes(image)


def makedisk_module():
    path = ROOT / 'tools' / 'makedisk.py'
    spec = importlib.util.spec_from_file_location('makedisk_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def header_lists():
    """The two role lists, read out of the header the stub and the kernel both compile against."""
    text = (ROOT / 'kernel' / 'include' / 'boot.h').read_text()
    out = {}
    for key in ('NAMES', 'FILES'):
        m = re.search(r'#define\s+BOOT_FIRMWARE_ROLE_' + key + r'\s+(\{[^}]*\})', text)
        assert m, 'no BOOT_FIRMWARE_ROLE_%s in kernel/include/boot.h' % key
        out[key] = re.findall(r'"([^"]*)"', m.group(1))
    return out


def test_the_examiner_compiles_the_way_the_stub_compiles_it():
    """The host harness is the same translation unit the boot application links, with the stub's own
    include path, so a check that passes here is a check about the code that runs before the kernel."""
    compile_line = (ROOT / 'Makefile').read_text()
    assert re.search(r'\$\(BUILD\)/efi-gsp_fw\.o: boot/uefi/gsp_fw\.c boot/uefi/gsp_fw\.h '
                     r'kernel/include/boot\.h Makefile', compile_line), \
        'the examiner must be compiled by the same rule that compiles it into BOOTX64.EFI, with boot.h ' \
        'as a dependency, or the two halves can be tested and shipped from different sources'
    assert 'efi-gsp_fw.o' in re.search(r'scos\.img.*|BOOTX64\.EFI: .*', compile_line, re.S).group(0), \
        'the examiner is built but never linked, which means no boot has ever run it'


def test_the_host_harness_runs_every_branch():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'gsp_fw_host'
        run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-Iboot/uefi',
                                      'tools/tests/gsp_fw_host.c', '-o', str(binary)])
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert not result.stderr.strip(), 'the sanitizers said something: ' + result.stderr
        m = re.match(r'^(\d+) checks, (\d+) failures$', result.stdout.strip())
        assert m, 'the harness did not report its count: ' + result.stdout[-500:]
        assert int(m.group(1)) == CHECKS, \
            'the harness reports %s checks, pinned at %d: a case removed is a case nobody noticed' % (
                m.group(1), CHECKS)
        assert int(m.group(2)) == 0, result.stdout


def test_the_five_names_are_one_list_in_three_places():
    """The stub reads a file by name, makedisk writes a file by name, and the panel tells a human the names.
    Three places, one meaning: this is the test that makes a half-done rename impossible."""
    lists = header_lists()
    assert lists['NAMES'] == ['gsp', 'fmc', 'bootloader', 'booter load', 'booter unload'], lists['NAMES']
    assert lists['FILES'] == ['GSP.BIN', 'GSPFMC.BIN', 'GSPBL.BIN', 'GSPBTL.BIN', 'GSPBTU.BIN'], \
        lists['FILES']
    module = makedisk_module()
    assert [r[1] for r in module.FIRMWARE_ROLES] == lists['FILES'], \
        'tools/makedisk.py writes names the boot stub will not read'
    assert [r[0] for r in module.FIRMWARE_ROLES] == lists['NAMES'], \
        'the intake and the report disagree about what the roles are called'
    # The upstream patterns the intake matches, in role order, are the ones the driver package ships.
    assert [r[2] for r in module.FIRMWARE_ROLES] == ['gsp', 'fmc', 'bootloader', 'booter_load',
                                                     'booter_unload'], \
        'the accepted upstream spellings changed; the panel sentence quoting them has to change with them'
    panel = (ROOT / 'kernel' / 'desktop' / 'graphics.c').read_text()
    for esp in lists['FILES']:
        assert esp in panel, 'the panel never names %s, so nobody can be told to stage it' % esp
    for name in module.FIRMWARE_ROLES:
        assert len(name[1].partition('.')[0]) <= 8 and len(name[1].partition('.')[2]) <= 3, \
            'FAT cannot hold %s, and makedisk would raise at image time rather than here' % name[1]


def test_the_handoff_grew_by_appending_and_says_so():
    """A firmware record added to the boot handoff is only safe if nothing a v4 kernel read moved.  The
    assertions in the header are the proof; this test refuses their removal or softening."""
    text = (ROOT / 'kernel' / 'include' / 'boot.h').read_text()
    assert re.search(r'#define BOOT_VERSION 6', text), 'the handoff version did not move with the ABI'
    for size, want, why in ((r'sizeof\(struct boot_handoff\)', '424', 'handoff ABI v6'),
                            (r'sizeof\(struct boot_firmware_role\)', '32', 'firmware record entry ABI'),
                            (r'sizeof\(struct boot_firmware\)', '216', 'firmware record ABI'),
                            (r'offsetof\(struct boot_handoff,firmware\)', '208',
                             'firmware record follows v4 fields unchanged')):
        m = re.search(r'_Static_assert\(' + size + r'==' + want + r',"([^"]*)"\)', text)
        assert m and not m.group(0).lstrip().startswith('//'), \
            'the assertion "%s" is missing or commented out: %s' % (why, why)
    assert '_Static_assert(offsetof(struct boot_handoff,framebuffer)==88' in text, \
           'the framebuffer offset the whole panel is written against lost its assertion'
    # v6 appended the copy the kernel is to read the bytes from, which is where `offset' in each role came
    # from; the record grew from 200 to 216 and pulled the whole structure with it because a uint64_t has to
    # start on an 8-byte boundary.  Pinned here as well as in the header, because that growth is the one way
    # this append could have silently moved something a v5 reader was already counting on.
    assert 'BOOT_FIRMWARE_RESIDENT_MAX' in text and 'uint32_t offset' in text, \
        'the resident window lost its cap or its per-role offset, which is what keeps a lying size field from ' \
        'turning into a copy past the end of the allocation'
    # The record has to be the last field, or "appended" stops meaning what the comment claims.
    body = text[text.index('struct boot_handoff {'):text.index('_Static_assert')]
    assert body.rstrip().endswith('};') and 'firmware;' in body and body.rindex('firmware;') > \
        body.rindex('module_name'), 'the firmware record is not the tail of the handoff'


def test_the_kernel_reads_the_fmc_contract_the_loader_writes():
    """The kernel half, on a host: the shape rules from gh100.c applied to bytes somebody could tamper with.
    Compiled against kernel/drivers/gpu/gpu_firmware.c itself under ASan and UBSan, so a section header that
    points past the end of its image is caught as a read out of bounds rather than as a wrong answer.  The two
    files the python builders above produce are run through the same parser, which is what keeps the C harness
    and this one describing the same container."""
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'gsp_fw_kernel_host'
        run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-Ikernel/include', 'tools/tests/gsp_fw_kernel_host.c', '-o', str(binary)])
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        assert not result.stderr.strip(), 'the sanitizers said something: ' + result.stderr
        m = re.match(r'^(\d+) check\(s\), (\d+) failure\(s\)$', result.stdout.strip())
        assert m, 'the kernel harness did not report its count: ' + result.stdout[-400:]
        assert int(m.group(1)) == KERNEL_CHECKS, \
            'the kernel harness reports %s checks, pinned at %d' % (m.group(1), KERNEL_CHECKS)
        assert int(m.group(2)) == 0, result.stdout

        # Each mode then has to refuse a damaged file, or the checks above only prove the parser is
        # permissive.  The damage differs by container because the two are judged by different rules: an FMC
        # image by the CRC32 each section carries, the RM image by nothing of the kind - so for that one the
        # byte changed is the class byte, which is what both halves agree has to say ELF64.
        for mode, blob, poke, what in (
                ('--fmc', build_fmc_image(), lambda b: b.__setitem__(330, b[330] ^ 0xff),
                 'an FMC image as the driver builds one'),
                ('--rm', build_container(), lambda b: b.__setitem__(4, 3),
                 'the RM image, with .fwimage and a signature')):
            for expect_ok, mutate in ((True, None), (False, poke)):
                image = bytearray(blob)
                if mutate:
                    mutate(image)
                path = Path(tmp) / 'image.bin'
                path.write_bytes(bytes(image))
                result = subprocess.run([str(binary), mode, str(path)], cwd=ROOT, capture_output=True,
                                        text=True)
                if expect_ok:
                    assert result.returncode == 0, '%s (%s) did not parse as the format says it should: %s' % (
                        mode, what, result.stdout + result.stderr)
                else:
                    assert result.returncode != 0, 'a one-byte change inside %s was accepted by %s, so %s is ' \
                        'not being checked: %s' % (what, mode, 'the section CRC32' if mode == '--fmc'
                                                   else 'the class byte', result.stdout)


def test_a_staged_set_reaches_the_panel_on_a_booted_machine():
    """The order is not stylistic: the boot volume handle is closed before the kernel exists, and a read
    attempted afterwards is a use-after-close in code with no fault handler."""
    main = (ROOT / 'boot' / 'uefi' / 'main.c').read_text()
    read = main.index('for(unsigned role=0;role<BOOT_FIRMWARE_ROLES;role++)')
    close = main.index('status close=root->close(root)')
    handoff = main.index('h->firmware=gsp_fw')
    assert read < close, 'the firmware loop moved after the boot volume closes, where it can only fail'
    assert handoff > close, 'the record must be written into the handoff arena, which exists later'
    assert main.count('bs->free_pool(blob)') == 1, \
        'every image read has to be released: the pool is firmware memory, and a multi-megabyte leak in ' \
        'the boot stub is memory the kernel never gets back'
    assert 'BOOT_FIRMWARE_STATE_UNREADABLE' in main, \
        'a file the firmware could not open must not be reported as absent'


def test_the_intake_refuses_what_it_cannot_carry():
    """The copy step has to be as careful as the parser: silently shipping the wrong chip's image, or an
    empty one, or one the stub will fail to read, is a disk that cannot be reasoned about."""
    module = makedisk_module()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        assert module.firmware_blobs(root) == [], 'a disk with no gsp directory is the normal case, not an error'

        (root / 'gsp').mkdir()
        good = build_container()
        (root / 'gsp' / 'GSP.BIN').write_bytes(good)
        assert module.firmware_blobs(root) == [('GSP.BIN', good)], 'the exact ESP name was not taken as-is'

        # The upstream spelling, in the directory layout a driver package actually has.
        shutil.rmtree(root / 'gsp')
        nested = root / 'gsp' / 'nvidia' / 'gb207' / 'gsp'
        nested.mkdir(parents=True)
        (nested / 'gsp-570.144.bin').write_bytes(good)
        (nested / 'fmc-570.144.bin').write_bytes(b'\0' * 4096)
        found = dict(module.firmware_blobs(root))
        assert found['GSP.BIN'] == good and found['GSPFMC.BIN'] == b'\0' * 4096, \
            'an unpacked package should not have to be renamed by hand: %s' % sorted(found)
        assert len(found) == 2, 'the intake invented roles the disk does not hold: %s' % sorted(found)

        # Two chips on one stick: refused, because guessing is what this directory exists to prevent.
        (root / 'gsp' / 'nvidia' / 'gb202' / 'gsp').mkdir(parents=True)
        (root / 'gsp' / 'nvidia' / 'gb202' / 'gsp' / 'gsp-570.144.bin').write_bytes(good + b'x')
        try:
            module.firmware_blobs(root)
            raise AssertionError('two candidate images for one role were accepted')
        except ValueError as exc:
            assert 'leave only the image for the chip' in str(exc), exc

        # Oversized and empty: both refused here, not on a panel 1000 miles from a shell.
        shutil.rmtree(root / 'gsp')
        (root / 'gsp').mkdir()
        (root / 'gsp' / 'GSP.BIN').write_bytes(b'\0' * (module.FIRMWARE_MAX + 1))
        try:
            module.firmware_blobs(root)
            raise AssertionError('a file past the stub read cap was accepted')
        except ValueError as exc:
            assert 'past the boot stub read cap' in str(exc), exc
        (root / 'gsp' / 'GSP.BIN').write_bytes(b'')
        try:
            module.firmware_blobs(root)
            raise AssertionError('an empty image was accepted')
        except ValueError as exc:
            assert 'is empty' in str(exc), exc


def test_the_container_the_intake_accepts_is_the_one_the_loader_reads():
    """makedisk does not parse anything, so the container has to be right before it is staged.  This checks
    the builder the boot half uses against the same rules the C harness pins, so the two cannot drift into
    testing different shapes."""
    blob = build_container()
    assert blob[:4] == b'\x7fELF' and blob[4] == 2 and blob[5] == 1, 'not a 64-bit little-endian container'
    shoff = struct.unpack_from('<Q', blob, 40)[0]
    shentsize, shnum, strndx = struct.unpack_from('<HHH', blob, 58)
    assert (shentsize, shnum, strndx) == (64, 4, 3), \
        'the container the boot half stages is not the four-section shape the harness pins'
    string_at, string_size = struct.unpack_from('<QQ', blob, shoff + 3 * 64 + 24)
    names = blob[string_at:string_at + string_size]
    assert b'.fwimage' in names and b'.fwsignature_gb20x' in names, names
    # A lie in one field has to survive the walk rather than be believed: the truncated case is the shape of
    # a copy interrupted halfway, which is the most likely real damage to a stick.
    assert build_container(shnum=4000) and build_container(shoff=1 << 32)
    # And the two builders - this one in python, the harness's in C - have to describe the same container,
    # because the boot half below stages a python-built file and asserts what the C examiner says about it.
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'gsp_fw_host'
        run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-Iboot/uefi',
                                     'tools/tests/gsp_fw_host.c', '-o', str(binary)])
        for blob, expectation in ((blob, 0), (build_container(signature=False), 1)):
            path = Path(tmp) / 'container.bin'
            path.write_bytes(blob)
            result = subprocess.run([str(binary), '--check', str(path)], cwd=ROOT,
                                    capture_output=True, text=True)
            if expectation == 0:
                assert result.returncode == 0, 'a container built here does not examine as the boot ' \
                    'sequence would read it: ' + result.stdout + result.stderr
                assert ', 0 failures (' in result.stdout, result.stdout
                assert 'container ' in result.stdout and '1896 B' in result.stdout, result.stdout
            else:
                assert result.returncode != 0, \
                    'a container with no signature section passed a check that claims to require one'


def test_a_staged_set_reaches_the_panel_on_a_booted_machine():
    """The stub, the FAT volume, the handoff and the panel together.  A container built here is read by the
    real firmware off a real disk image, and the numbers on the panel have to be the numbers the file
    actually holds - which is the only way this feature can be tested at all before the card in question is
    in front of somebody."""
    sys.path.insert(0, str(ROOT / 'tools' / 'tests'))
    from qemu_support import Guest, restore_staged, stage_aside
    build = ROOT / 'build'
    gsp = build / 'gsp'
    held = stage_aside()
    nested = gsp / 'nvidia' / 'gb207' / 'gsp'
    good = build_container()
    fmc = build_fmc_image()
    others = {name: bytes((i * 3) & 0xff for i in range(4096))
              for name in ('bootloader-570.144.bin', 'booter_load-570.144.bin', 'booter_unload-570.144.bin')}
    others['fmc-570.144.bin'] = fmc
    try:
        nested.mkdir(parents=True)
        (nested / 'gsp-570.144.bin').write_bytes(good)
        for name, blob in others.items():
            (nested / name).write_bytes(blob)
        run(['python3', 'tools/makedisk.py', 'build'])
        with Guest(name='firmware-complete', vga='cirrus') as g:
            g.run(1.2)
            serial = g.serial()
            assert 'wm: entering main loop' in serial, serial[-900:]
            assert 'GPU could not be initialized' not in serial, serial[-900:]
            # The kernel's log line: short by design, because klog truncates near 120 bytes and a line that
            # lost its tail would be a worse report than no line at all.
            line = [l for l in serial.splitlines() if 'gsp firmware' in l]
            assert line, 'the boot log never says what the stub found: ' + serial[-900:]
            assert '5 of 5 images read by the stub' in line[0], line[0]
            assert 'set complete' in line[0], line[0]
            assert 'disagrees with the per-file records' not in serial, \
                   "the kernel recomputed the stub's arithmetic and got a different answer: " + line[0]
            cap = REPORT_CAP    # the capacity the terminal and the tty hand to graphics_report()
            g.call('graphics_report', g.scratch, cap)
            panel = g.string(g.scratch, cap)
            assert len(panel) < cap, \
                   'the report fills the buffer it is given, so something below this line was clipped: ' \
                   + panel[-200:]
            # The whole paragraph has to arrive in the buffer the panel actually has: on a real card the
            # module adds its own 1152-byte inventory above, so headroom here is measured, not assumed.
            assert 'GSP firmware: 5 of 5 image(s) staged' in panel, panel[-1500:]
            assert 'set complete for the boot sequence to ask for' in panel, panel[-1500:]
            assert str(len(good)) + ' B' in panel, "the staged container's size never reached the panel"
            assert 'ELF64 4s +.fwimage 1024 +.fwsignature_gb20x 512' in panel, \
                   'the sections the boot sequence reads are not reported as it read them: ' \
                   + panel[-1200:]
            assert 'crc32 0x' in panel, 'no checksum to compare against the copy on another machine'
            assert 'not what it can do to the card - the paragraph below says which of the ' in panel, \
                   'the intake paragraph claims more or less than the handover does: ' + panel[-1400:]

            # The all-or-nothing tail, exercised for real: called with a buffer too small for the three tail
            # paragraphs, the report must say so with both numbers and point at the narrow command - and must
            # not be a clipped copy of the same text.  This is the branch that decides whether a long panel on a
            # real card arrives whole or arrives halfway, so it is tested against a booted kernel rather than
            # against the source.
            # The front line, measured rather than asserted: it has to be short enough to survive the copy a
            # person can actually make, and it has to still be there when the report is asked for in 1400 bytes,
            # because 1400 is roughly what three real machines have managed to deliver.
            line = panel.splitlines()[0]
            assert line.startswith('FSP: '), 'the report no longer opens with the summary line: ' + panel[:200]
            assert len(line) <= 512, 'the headline is %d bytes, which is not a line, it is a paragraph' % len(line)
            g.call('graphics_report', g.scratch, 1400)
            short = g.string(g.scratch, 1400)
            assert short.startswith(line), \
                   'a 1400-byte copy lost the first line: %r vs %r' % (short[:120], line[:120])
            # On a cirrus display function the honest answer is that nothing was asked, and the line has to say
            # that in itself rather than leave a reader to wonder whether the probe was skipped or lost: the two
            # are different facts, and on a real card the same slot holds the boot scratch and the reply's error.
            assert 'not asked' in line or 'graphics fsp' in line, \
                   'the summary neither answers nor points at the answer: ' + line
            assert line in panel and panel.index(line) == 0, \
                   'the line the panel opens with is not the line that was built: %r' % panel[:120]
            cut = panel.index('GSP firmware:')         # where the three tail paragraphs begin
            spare = len(panel) - cut                   # everything the guard has to fit, plus what follows it
            assert spare > 800, 'the tail this guard exists for is only %d bytes: the paragraphs have been ' \
                               'shortened somewhere, which is the thing being tested' % spare
            half = cut + 1000                          # room for some of it, which is the failure mode
            g.call('graphics_report', g.scratch, half)
            tight = g.string(g.scratch, half)
            assert 'Firmware intake and FSP mailbox: not printed' in tight, \
                   'a buffer with room for 1000 of the tail printed a clipped copy instead of declining: ' \
                   + tight[-400:]
            assert 'graphics fsp' in tight, \
                   'the notice does not say where the rest went: ' + tight[tight.index('not printed'):][:400]
            # And the numbers in that notice have to be the kernel's own measurement, in the right relation:
            # it declines only when what is needed is more than what is left.  Read them back rather than
            # restating them here, because the tail's length is a property of the card, not of this test.
            stated = re.search(r'They need (\d+) bytes and this buffer has only (\d+) left', tight)
            assert stated, 'the notice does not carry both numbers: ' + tight[tight.index('not printed'):][:400]
            assert int(stated.group(1)) > int(stated.group(2)), \
                   'it declined a tail of %s bytes that would have fitted in %s' % stated.groups()
            assert 'GSP firmware: 5 of 5' not in tight, 'the clipped call printed the paragraphs anyway'
            assert tight.startswith(panel[:40]), \
                   'the report above the guard changed between the two calls, so one of them measured a ' \
                   'different boot'

            g.call('graphics_fsp_report', g.scratch, cap)
            narrow = g.string(g.scratch, cap)
            assert 'GSP firmware: 5 of 5 image(s) staged' in narrow and 'FSP mailbox: ' in narrow, \
                   'the narrow report does not carry the paragraphs it was written for: ' + narrow[:400]
            assert 'Renderer:' not in narrow and 'Detection' not in narrow, \
                   'the narrow report is the whole panel under another name, which is the truncation it ' \
                   'exists to avoid'
            assert len(narrow) < cap, 'the narrow report fills its buffer, which is the failure it prevents'
            # Headroom, measured rather than assumed: the longest thing above this paragraph is a driver
            # module's own inventory, bounded by gpu_module_state's describe[1152] on a real card.  If the
            # report plus that much text does not fit the caller's buffer, the panel a user pastes is missing
            # the tail of one of the two, and no amount of prose in a doc would catch it later.
            assert cap - len(panel) > 1152, \
                   'only %d bytes of the %d-byte report buffer are spare, which a full engine inventory ' \
                   'will not fit in' % (cap - len(panel), cap)
            # The sentence at the end of the intake paragraph, not the end of the report: more lines follow
            # it (the path clause, the table counts, the scanout owner), so "in panel" plus the length check
            # above is what proves nothing between them was dropped.
            assert 'nothing here claims to have approved one.' in panel, \
                   'the intake paragraph is clipped inside the report buffer: ' + panel[-400:]
            assert 'notification: GSP firmware read from the boot disk' in serial, serial[-900:]
            # The stub saw a 32-bit container in the fmc role and said so, rather than reporting the real
            # FMC image as no container at all because a 64-bit check was pointed at it.
            assert 'fmc %d B crc32 0x' % len(fmc) in panel, panel[-1600:]
            assert ', ELF32 6s' in panel, 'the boot stub did not recognise the FMC image as a container'
            # What this kernel read out of the bytes it was given, in memory it is holding.
            assert 'GSP firmware image: 5 of 5 held read-only at 0x' in panel, panel[-1800:]
            # The card's own security processor, on a machine whose display function is not the one these
            # offsets decode inside.  The paragraph's whole job on such a boot is to say so by name - which
            # vendor's card this turned out to be, and that nothing of it was touched - because a panel that
            # simply omitted the paragraph could not be told apart from a kernel that forgot to look.
            assert 'FSP mailbox: not asked, and not because anything failed' in panel, panel[-2400:]
            assert 'vendor 0x00001013' in panel, 'the paragraph never names whose card this is: ' + panel[-2400:]
            assert "so no byte of this function's BAR was read, and none was written" in panel, panel[-2400:]
            assert ('fmc: ELF32 %d B, 6 sections, FMC header exact, every section legal and its CRC32 '
                    'verified, 0 CRC32 mismatch(es); image 512 B at 0x' % len(fmc)) in panel, panel[-1800:]
            assert 'hash 32, publickey 4, signature 256 B - the four numbers the FSP verify command is given' \
                in panel, panel[-1800:]
            assert 'gsp: ELF64 %d B, 4 sections, .fwimage 1024 B at 0x' % len(good) in panel, panel[-1800:]
            assert '.fwsignature_gb20x 512 B at 0x' in panel, panel[-1800:]
            assert 'every image the boot sequence kept was parsed' in panel, panel[-1800:]
            assert 'Nothing was written to the card' in panel, panel[-1800:]
            assert 'image(s) held at 0x' in serial, 'the kernel log never says the bytes were kept: ' + serial[-900:]
            print('   measured report length with a complete set staged: %d bytes' % len(panel))
    finally:
        shutil.rmtree(gsp, ignore_errors=True)
        run(['python3', 'tools/makedisk.py', 'build'])

    # The same machine with a file that is not a container, and four roles missing: this is what a bad copy
    # or a half-finished download looks like, and the OS is required to boot anyway and say what it saw.
    try:
        nested.mkdir(parents=True)
        (nested / 'gsp-570.144.bin').write_bytes(
            b'<!DOCTYPE html>\n<a href="https://example.invalid/download">Sign in to continue</a>\n')
        (nested / 'fmc-570.144.bin').write_bytes(b'not an image either')
        run(['python3', 'tools/makedisk.py', 'build'])
        with Guest(name='firmware-hostile', vga='cirrus') as g:
            cap = REPORT_CAP    # as above, read from the header rather than restated
            g.run(1.2)
            serial = g.serial()
            assert 'wm: entering main loop' in serial, serial[-900:]
            assert 'incompatible UEFI handoff' not in serial, serial[-900:]
            g.call('graphics_report', g.scratch, cap)
            panel = g.string(g.scratch, cap)
            assert len(panel) < cap, 'clipped: ' + panel[-200:]
            assert 'GSP firmware: 2 of 5 image(s) staged' in panel, panel[-1500:]
            assert 'set incomplete' in panel, panel[-1500:]
            assert 'no container' in panel and 'a text file, an HTML download page, or a copy ' in panel, panel[-1400:]
            assert 'interrupted halfway' in panel, panel[-1400:]
            assert 'Not on the disk: bootloader, booter load, booter unload' in panel, panel[-1400:]
            assert 'nothing here claims to have approved one.' in panel, panel[-400:]
    finally:
        shutil.rmtree(gsp, ignore_errors=True)
        restore_staged(held)
        run(['python3', 'tools/makedisk.py', 'build'])


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for name, fn in tests:
        fn()
    print('PASS: the firmware examiner, the five names, the handoff that carries the record, the intake '
          'that refuses a bad file, and two booted machines that read the result off a real FAT volume')


if __name__ == '__main__':
    main()
