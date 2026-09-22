"""GPU auto-detection regression: generated tables, host harness, emulated PCI.

Three layers, because each catches a different failure:

1. `tools/research/gen_gpu_tables.py --check` - the ID tables in the tree are still
   what the pinned upstream drivers bind by.  Skipped with a clear reason when no
   Haiku checkout is available (it is a ~90 MB sparse tree, fetched by
   `tools/research/audit_basic_2d_sources.py --fetch` or a plain sparse clone).
2. `tools/tests/gpu_detect_sim.c` - the *shipped* detection sources compiled for the
   host against a simulated PCI bus.  This is what proves exact matching for every
   family and both ends of every table: QEMU has no Radeon, Matrox, i810 or S3
   device model and cannot re-identify a function, so those cases cannot be run as
   emulated hardware at all.
3. QEMU with real emulated PCI display functions attached beside the boot one -
   `ati-vga`, `cirrus-vga`, `virtio-vga`.  This proves the same code runs inside the
   actual kernel image, that the ATI device is matched by its real ID through the
   generated table, that unmatched devices stay unmatched, that no PCI configuration
   write happens, and that the console survives being enumerated.
4. The same image with a tampered driver module: `build/scos.img` is edited in place (a payload byte
   flipped, and DRVLIST.IDX's crc for that file recomputed so the boot stub's check still passes) and
   booted, which proves the kernel validates the module on its own terms and that a refused driver
   cannot take the desktop down with it.
"""
import os, re, shlex, struct, subprocess, sys, zlib
from pathlib import Path
import importlib
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_support import Guest, RESULTS

ROOT = Path(__file__).resolve().parents[2]
HAIKU = Path(os.environ.get('SCOS_HAIKU', '/home/user/gpu-2d/sources/haiku'))
FLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra',
         '-Wno-builtin-declaration-mismatch', '-Ikernel/include']
# gpu_module is deliberately not in the host harness: the loader calls the page allocator, the device mapper and
# BAR sizing, which only exist in a booted kernel.  Its predicates are covered by the packer's own
# --verify (same checks, same order) and by the QEMU layer below, which boots a real image.
KERNEL_UNITS = ['gpu_detect', 'gpu_match', 'gpu_ports']

# QEMU's `-device ati-vga` presents the Rage 128 PCI function whose ID Haiku's `ati`
# driver binds.  `bochs-display` is deliberately absent: with a second framebuffer of
# the same shape OVMF no longer boots from the disk, so it cannot be attached here.
CASES = [
    ('ati-vga', 0x1002, 0x5046, 'ati', 'RAGE 128 PRO GL'),
    ('cirrus-vga', 0x1013, 0x00b8, None, None),
    ('virtio-vga', 0x1af4, 0x1050, None, None),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def test_generated_tables():
    if not (HAIKU / '.git').exists():
        print(f'skip: no Haiku checkout at {HAIKU}; set SCOS_HAIKU to verify the tables')
        return
    result = run(['python3', 'tools/research/gen_gpu_tables.py', '--haiku', str(HAIKU),
                  '--out', 'kernel/drivers/gpu/gpu_ids.h', '--check'])
    print(result.stdout.strip() or result.stderr.strip())
    assert result.returncode == 0, 'generated ID tables are stale or hand-edited'


def test_registry_naming_header():
    """`gpu_ids_registry.h` is regenerated from an in-tree snapshot, so it can be verified with no
    network and no Haiku checkout - which matters because the whole point of that table is chips that
    postdate every driver in the tree."""
    r = subprocess.run([sys.executable, 'tools/research/gen_gpu_tables.py', '--registry-only',
                        '--out', 'kernel/drivers/gpu/gpu_ids_registry.h', '--check'],
                       cwd=ROOT, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    print(r.stdout.strip())
    reg = (ROOT / 'kernel/drivers/gpu/gpu_ids_registry.h').read_text()
    drv = (ROOT / 'kernel/drivers/gpu/gpu_ids.h').read_text()
    # A naming row for the machine this work was asked for, and the invariant that the two tables
    # never overlap: an id bound by a driver must not also be filed as "known, nothing more".
    assert 'GB207 [GeForce RTX 5050] (Blackwell)' in reg, 'the registry table lost the RTX 5050 row'
    ROW = r'\{0x([0-9a-f]{1,4}), 0x([0-9a-f]{1,4}), "[^"]+"\}'
    reg_ids = {tuple(int(x, 16) for x in m) for m in re.findall(ROW, reg)}
    # Driver rows are the ones whose provenance comment names an upstream file, not the registry.
    drv_ids = {tuple(int(x, 16) for x in m) for m in re.findall(ROW + r', /\* (?!pci\.ids)', drv)}
    assert len(reg_ids) > 1200 and len(drv_ids) > 1000, (len(reg_ids), len(drv_ids))
    assert not (reg_ids & drv_ids), ('ids present in both tables: '
                                     + ', '.join(hex(d) for _, d in sorted(reg_ids & drv_ids)[:6]))


def test_host_harness():
    out = ROOT / 'build' / 'gpu-detect-host'
    out.parent.mkdir(exist_ok=True)
    sources = [ROOT / 'kernel' / 'drivers' / 'gpu' / (unit + '.c') for unit in KERNEL_UNITS]
    sources.append(ROOT / 'tools' / 'tests' / 'gpu_detect_sim.c')
    objects = []
    for source in sources:
        assert source.is_file(), source
        object_path = out.parent / ('host-%s.o' % source.stem)
        compiled = run(['gcc'] + FLAGS + ['-c', str(source), '-o', str(object_path)])
        assert compiled.returncode == 0, source.name + '\n' + compiled.stderr
        assert not compiled.stderr.strip(), source.name + '\n' + compiled.stderr
        objects.append(object_path)
    link = run(['gcc'] + [str(o) for o in objects] + ['-o', str(out)])
    assert link.returncode == 0, link.stderr
    executed = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True)
    print('\n'.join(line for line in executed.stdout.splitlines()
                    if not line.startswith('[klog]')))
    assert executed.returncode == 0, executed.stdout[-4000:] + executed.stderr[-2000:]


def pixels(g, name):
    path = RESULTS / (g.name + '-' + name + '.ppm')
    g.qmp('screendump', {'filename': str(path)})
    data = path.read_bytes()
    magic, width, height, maxval, body = data.split(None, 4)
    return body[:int(width) * int(height) * 3]


def test_tampered_module_is_refused():
    """The driver on the disk is only as trustworthy as the check that guards it.

    The boot stub verifies the module against `DRVLIST.IDX`, so the tamper below leaves that index
    consistent with the file: a corrupt card, a bad image write or a deliberate edit can all produce
    exactly that state.  What must still catch it is the kernel's own payload check, and what must still
    be true afterwards is a usable machine on the CPU compositor.
    """
    image = ROOT / 'build' / 'scos.img'
    module = ROOT / 'build' / 'gpu' / 'ati.mod'
    if not image.exists() or not module.exists():
        raise AssertionError('build/scos.img and build/gpu/ati.mod are required; run make first')
    original = image.read_bytes()
    clean = module.read_bytes()
    at = 192 + 512                       # inside the loaded blocks, clear of the header
    bad = bytearray(clean)
    bad[at] ^= 0x01
    needle = clean[at:at + 64]
    pos = original.find(needle)
    assert pos >= 0, 'module payload not found in the image (is the image older than the module?)'
    assert original.find(needle, pos + 1) < 0, 'module payload appears twice; refusing to guess'
    img = bytearray(original)
    img[pos] ^= 0x01
    # The index is `<II count,total>` followed by one `<8s NAME, I size, I crc32>` per driver (the
    # upper-case ESP spelling, which is also what the boot stub matches on), in the order the packer
    # put them on the disk.  It is located by rebuilding it from `build/gpu/*.mod`
    # rather than by assuming a single driver: the store has to describe every family that was packed,
    # and once more than one module ships, "the index matches the files" is itself worth testing.
    packed = sorted((ROOT / 'build' / 'gpu').glob('*.mod'))
    assert len(packed) >= 2, ('expected one module per driver family on the disk', [q.name for q in packed])
    blobs = {q: q.read_bytes() for q in packed}
    index = struct.pack('<II', len(packed), sum(len(b) for b in blobs.values()))
    for path in packed:
        blob = blobs[path]
        index += struct.pack('<8sII', path.stem.upper().encode()[:8].ljust(8, b'\0'), len(blob),
                             zlib.crc32(blob) & 0xffffffff)
    hpos = img.find(index)
    assert hpos >= 0, 'the driver index on the image does not describe build/gpu/*.mod'
    slot = packed.index(module)
    entry_at = hpos + 8 + 16 * slot
    size, recorded = struct.unpack('<II', bytes(img[entry_at + 8:entry_at + 16]))
    assert size == len(clean), (size, len(clean))
    assert recorded == (zlib.crc32(clean) & 0xffffffff), 'the index crc does not match the module on disk'
    # Rewrite that entry's crc so the stub accepts the tampered file, leaving only the kernel's own
    # check to refuse it.
    struct.pack_into('<I', img, entry_at + 12, zlib.crc32(bytes(bad)) & 0xffffffff)
    image.write_bytes(bytes(img))
    try:
        with Guest('gpu-tamper', devices=['ati-vga']) as g:
            log = g.serial()
            assert 'gpu: module rejected: payload CRC mismatch (the file is corrupt)' in log, log[-2500:]
            assert 'validated' not in log, log[-2500:]
            assert 'engine verified in device memory' not in log, log[-2500:]
            # The machine must still be a machine: the WM main loop, and no panic anywhere.
            assert 'wm: entering main loop' in log, log[-2500:]
            # The refusal must also reach the user, not just the log: that notice is the whole reason a
            # missing or refused driver is a legible state instead of a silently slow machine.
            assert 'No 2D engine bound' in log and 'No supported 2D engine was matched' in log, log[-2500:]
            g.call('graphics_report', g.scratch, 4096)
            report = g.string(g.scratch, 4096)
            print(report)
            assert 'unavailable (no driver module loaded for this chip)' in report, report
            assert 'verified by device readback' not in report, report
            # The stub's verdict and the kernel's are deliberately shown apart: the media passed the
            # first and failed the second, which is what this whole layer is demonstrating.
            assert 'module store: staged from' in report, report
            print('PASS: a tampered driver module is refused by the kernel while the desktop carries on')
    finally:
        image.write_bytes(original)
        restored = image.read_bytes() == original
        assert restored, 'could not restore build/scos.img after the tamper test'


def test_local_record_survives_regeneration():
    """The header is generated, except for one hand-authored family - and that exception is checked.

    `gen_gpu_tables.py --check` needs a Haiku checkout, which not every machine has, so the invariant
    that matters when nobody is regenerating is stated separately: the committed header must still hold
    the local Cirrus record byte for byte, define it once, and carry counts that agree with the table it
    sits in.  A regeneration that dropped the family, or an edit that left the counts behind, fails here.
    """
    check = subprocess.run([sys.executable, 'tools/research/gen_gpu_tables.py', '--check-local'],
                           cwd=ROOT, capture_output=True, text=True)
    print(check.stdout.strip())
    assert check.returncode == 0, check.stdout + check.stderr


def test_qemu_emulated_adapters():
    devices = [spec for spec, *_ in CASES]
    with Guest('gpu-detect', devices=devices) as g:
        log = g.serial()
        lines = [l for l in log.splitlines() if l.startswith('gpu:')]
        assert lines, log[-2000:]
        print('\n'.join(lines))

        # The detection summary and the console are both in the log: an adapter that is
        # named must not have been reprogrammed, and the desktop must still be alive.
        summary = next(l for l in lines if 'display function(s)' in l)
        unrecorded = int(re.search(r'(\d+) without a port record', summary).group(1))
        # The split counts must be visible on the serial log of a real boot too, not only in the
        # headers: it is the only place where a reader can see that most of the table is naming.
        tables = next(l for l in lines if 'id rules bind a driver' in l)
        counts = [int(x) for x in re.findall(r'(\d+)', tables.split('tables:', 1)[1])]
                # 1020 = the 1019 ids read out of the pinned upstream tables plus the one hand-authored Cirrus
        # row; 1296 is the naming table, which binds no driver.  Both numbers are printed by the boot
        # itself, so this checks the shipped image, not a rebuild of the header.
        assert counts[:2] == [1020, 1296], (counts, tables)
        assert unrecorded == 0, summary
        writes = next(l for l in lines if 'scanout owner' in l)
        assert '0 PCI config write(s) issued' in writes, writes
        detected = {'%x:%x' % (vendor, device): family
                    for spec, vendor, device, family, _ in CASES}
        for line in lines:
            match = re.search(r'PCI \d+:\d+\.\d+ ([0-9a-f]{1,4}:[0-9a-f]{1,4}) \(sub 0x[0-9a-f]+\) matched=(\S+)', line)
            if not match:
                continue
            ident, family = match.groups()
            if ident in detected:
                expected = detected[ident] or 'none'
                assert family == expected, (ident, family, expected)
        # The boot display is the QEMU/Bochs function, which no upstream family binds by ID.
        assert '1234:1111 sub=0 matched=none' in log, log[-1500:]

        assert g.call('gpu_config_writes_during_detect') == 0, 'detection wrote config space'
        assert g.call('gpu_bound_driver') == 0, 'nothing may be bound with no engine ported'
        assert g.call('gpu_device_count') >= 1 + len(CASES), 'an emulated adapter was missed'
        assert g.call('gpu_match_id_total') > 900, 'the generated tables did not link in'

        g.call('graphics_report', g.scratch, 4096)
        report = g.string(g.scratch, 4096)
        print(report)
        assert 'Detection is not driver support' in report, report
        assert 'config writes during detection: 0' in report, report
        assert 'family: ati' in report and 'chip: RAGE 128 PRO GL' in report, report

        # The module pipeline, measured in the guest rather than argued: the chip's family must have
        # been opened from \SCOS\, validated, relocated and *verified by readback* - a module that only
        # loaded proves nothing.  The accounting line is the point of one file per family, so it is
        # asserted here too: bytes that exist on the disk and were never touched.
        assert re.search(r'gpu: module \d+ module\(s\), \d+ B on the boot disk: \d+ B opened '
                         r'for this chip, \d+ B never read', log), log[-2500:]
        validated = [l for l in lines if ') validated' in l]
        assert len(validated) == 1, log[-2500:]
        bound = [l for l in lines if 'bound: engine verified in device memory' in l]
        assert len(bound) == 1, log[-3000:]
        tested = re.search(r'\((-?\d+)/(-?\d+) pixels\)', bound[0])
        assert tested and int(tested.group(1)) == int(tested.group(2)) > 0, bound[0]
        assert 'gpu: module rejected' not in log, log[-2500:]

        # In this VM the console belongs to the firmware's primary adapter, so the report must say the
        # engine was verified on the second function and must NOT claim the desktop is being painted by
        # it: an honest report is what makes the difference between "the driver works" and "the driver
        # works and is used" observable from the outside.
        assert 'GPU acceleration: driver module' in report, report
        assert 'verified by device readback' in report, report
        assert 'not used for output: another PCI function feeds this display' in report, report
        assert 'module engine verified on this function, but another PCI function feeds the console' \
            in report, report
        assert 'used for solid output rectangles' not in report, report
        assert 'driver module loaded from storage for this family' in report, report

        # The compositor boundary itself, not just the log: an engine must be reachable from the code
        # that decides whether to hand a rectangle to the GPU, and on this machine the answer to
        # "does the bound engine feed the console?" must be no - which is exactly why the desktop is
        # still drawn by the CPU here, and why that is the correct result rather than a missing feature.
        assert g.call('gpu_engine_available') == 1, 'the bound module never reached the engine boundary'
        assert g.call('gpu_engine_drives_output') == 0, 'the console belongs to the firmware adapter'
        # A fill the engine accepts must return success on a rectangle inside its own surface.
        assert g.call('gpu_engine_fill_rectangle', 976, 736, 16, 16, 0x00c0ffee) == 0
        assert g.call('gpu_engine_wait_idle') == 0
        assert 'match: none' in report, report
        assert 'Scanout owner' in report, report

        # Enumerating four display functions must leave the visible console untouched:
        # paint twice and compare, then require actual content on screen.
        g.call('paint_all'); first = pixels(g, 'after-boot')
        g.call('paint_all'); assert pixels(g, 'repaint') == first
        assert len(set(first)) > 3, 'the desktop is blank after detection'


def test():
    RESULTS.mkdir(parents=True, exist_ok=True)
    test_generated_tables()
    test_registry_naming_header()
    test_host_harness()
    if not (ROOT / '.tools' / 'qemu' / 'bin' / 'qemu-system-x86_64').exists():
        raise AssertionError('QEMU bundle missing; run tools/setup_qemu.py before this suite')
    test_local_record_survives_regeneration()
    test_qemu_emulated_adapters()
    test_tampered_module_is_refused()


if __name__ == '__main__':
    test()
    print('PASS: GPU auto-detection verified against generated tables, simulated PCI '
          'and emulated PCI display devices')
