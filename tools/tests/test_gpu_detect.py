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
"""
import os, re, shlex, struct, subprocess, sys
from pathlib import Path
import importlib
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_support import Guest, RESULTS

ROOT = Path(__file__).resolve().parents[2]
HAIKU = Path(os.environ.get('SCOS_HAIKU', '/home/user/gpu-2d/sources/haiku'))
FLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra',
         '-Wno-builtin-declaration-mismatch', '-Ikernel/include']
KERNEL_UNITS = ['gpu_detect', 'gpu_ports', 'gpu_tables']

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
        assert 'GPU acceleration: unavailable (no hardware backend linked)' in report
        assert 'Detection is not driver support' in report, report
        assert 'config writes during detection: 0' in report, report
        assert 'family: ati' in report and 'chip: RAGE 128 PRO GL' in report, report
        assert 'SCos port: not ported yet' in report, report
        assert 'before GPU work is possible:' in report, report
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
    test_host_harness()
    if not (ROOT / '.tools' / 'qemu' / 'bin' / 'qemu-system-x86_64').exists():
        raise AssertionError('QEMU bundle missing; run tools/setup_qemu.py before this suite')
    test_qemu_emulated_adapters()


if __name__ == '__main__':
    test()
    print('PASS: GPU auto-detection verified against generated tables, simulated PCI '
          'and emulated PCI display devices')
