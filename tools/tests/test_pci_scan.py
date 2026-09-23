#!/usr/bin/env python3
"""Compile and run the boot stub's PCI display scan against a synthetic config space.

The scan decides which single driver module the firmware stages, and on the machine this project is being
written for - a GeForce RTX 5050 on bus 1 behind a PCIe root port - it is the step that went wrong: the
loop over a bridge's bus range excluded the very bus the port links, so the stub saw no display function,
staged no module, and the kernel left the registers to a driver that was never going to arrive.  QEMU puts
its display device on bus 0 and cannot reproduce that, which is why the walk is tested here instead.
"""
import subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror',
         '-Ikernel/include', '-Iboot/uefi']
OUTCOMES = {}


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def test_the_walk_is_header_only_and_its_bounds_are_its_own():
    """Static facts: the stub must not keep a private copy of the walk, and the walk must read only headers."""
    main = (ROOT / 'boot/uefi/main.c').read_text()
    assert '#include "pci_scan.h"' in main, 'the stub no longer uses the shared walk'
    assert 'for(uint8_t slot=0;slot<32;slot++)' not in main, \
        'the stub grew a second PCI loop, which is how two scanners drift apart'
    assert '.pci_functions_seen=pci_functions_seen' in main, \
        'the stub must hand its scan counts to the kernel, or the panel cannot say what it saw'
    scan = (ROOT / 'boot/uefi/pci_scan.c').read_text()
    assert 'next <= subordinate' in scan, \
        "a bridge's bus range is inclusive: excluding the endpoints is the bug this file exists to prevent"
    loops = [line for line in scan.splitlines() if 'for (unsigned next' in line]
    assert loops and 'secondary;' in loops[0].replace(' ', '') and '<=subordinate' in loops[0].replace(' ', ''), \
        'the loop over a bridge\'s bus range must start at the secondary bus and include the subordinate one'
    assert 'header & 0x80u' not in scan, \
        'the multifunction bit is in register 0x0e, not in the low byte of the dword at 0x0c'
    for write in ('out32(', 'pci_write', '->write'):
        assert write not in scan, f'the boot scan must read configuration space only ({write})'
    makefile = (ROOT / 'Makefile').read_text()
    assert '$(BUILD)/efi-pci_scan.o' in makefile, 'the walk is not linked into the stub'


def test_the_handoff_carries_the_scan_counts():
    """The kernel refuses a handoff of the wrong size, so the ABI note and the assert must agree."""
    boot = (ROOT / 'kernel/include/boot.h').read_text()
    assert '#define BOOT_VERSION 4' in boot, 'the handoff grew and its version was not raised'
    assert 'pci_functions_seen,pci_buses_scanned,pci_display_functions' in boot
    assert 'sizeof(struct boot_handoff)==208' in boot, \
        'the size the stub writes must be the size the kernel accepts'


def test_host_harness():
    out = ROOT / 'build' / 'pci-scan-host'
    out.parent.mkdir(exist_ok=True)
    sources = [ROOT / 'boot/uefi/pci_scan.c', ROOT / 'kernel/drivers/gpu/gpu_match.c',
               ROOT / 'tools/tests/pci_scan_host.c']
    objects = []
    for source in sources:
        assert source.is_file(), source
        obj = out.parent / ('pci-scan-%s.o' % source.stem)
        compiled = run(['gcc'] + FLAGS + ['-c', str(source), '-o', str(obj)])
        assert compiled.returncode == 0, source.name + '\n' + compiled.stderr
        assert not compiled.stderr.strip(), source.name + '\n' + compiled.stderr
        objects.append(obj)
    link = run(['gcc'] + [str(o) for o in objects] + ['-o', str(out)])
    assert link.returncode == 0, link.stderr
    # A firmware that describes a bridge badly is the case that could hang a boot, so the harness is run
    # under a timeout: the test fails loudly instead of waiting for a machine nobody is watching.
    executed = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True, timeout=120)
    print(executed.stdout.strip())
    assert executed.returncode == 0, executed.stdout[-4000:] + executed.stderr[-2000:]
    lines = [line for line in executed.stdout.splitlines() if line.startswith('PASS: ')]
    assert len(lines) >= 15, executed.stdout
    assert any('GeForce RTX 5050 on the far side of a root port' in line for line in lines), executed.stdout
    print(f'PASS: the boot stub\'s PCI walk found the RTX 5050 behind its root port, {len(lines)} checks')


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_') and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
        except AssertionError as exc:
            failed += 1
            print(f'FAIL: {t.__name__}: {exc}')
        except subprocess.TimeoutExpired:
            failed += 1
            print(f'FAIL: {t.__name__}: the scan did not terminate within 120 s')
    print(('FAIL: %d of %d' % (failed, len(tests))) if failed else
          ('PASS: PCI display scan, %d test(s)' % len(tests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
