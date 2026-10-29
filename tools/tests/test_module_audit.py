#!/usr/bin/env python3
"""The build-time cross-check that weighs a packed GPU module against the objects it was linked from.

A `.mod' certifies itself: its header checksum covers its header and its payload CRC covers its payload, so a
module packed from a truncated, stale or half-written object is internally perfect, passes every predicate the
boot stub applies, loads, and then does nothing.  That is not hypothetical: a suite that packs a throwaway
module in place, rebuilt the image, and died before restoring, left the throwaway in `build/gpu/' with a newer
timestamp than its object - `make' found nothing to redo, and the next image would have shipped a Cirrus
driver of 600 bytes instead of 7216.  The objects in `build/' are the only other witness to what a family's
driver weighs, which is what this compares.  The check has to fire on the mismatch and stay quiet when there is
nothing to compare against, because a verification that invents a failure on a tree that was not built is its
own kind of false report.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = str(ROOT / 'tools' / 'build_gpu_module.py')
MODULE_FLAGS = ['-Ibuild', '-m64', '-ffreestanding', '-fno-stack-protector', '-fno-builtin',
                '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-mgeneral-regs-only', '-O2',
                '-Wall', '-Wextra', '-Werror', '-std=c11', '-fno-pie', '-mcmodel=small',
                '-fno-strict-aliasing']


def run(*args):
    return subprocess.run([sys.executable if args[0] == 'python3' else args[0]] + list(args[1:]),
                          cwd=ROOT, capture_output=True, text=True)


def compile_stub(directory, name):
    """A Cirrus-shaped module with almost nothing in it - the throwaway the identification suite builds, so this
    fixture is the exact shape that left a stub in build/gpu/ once.  Returned as the linked object the packer is
    given, the way the Makefile produces `build/gpu/<family>.o'."""
    sys.path.insert(0, str(ROOT / 'tools' / 'tests'))
    import test_nvidia_ident
    source = directory / (name + '.c')
    source.write_text(test_nvidia_ident.SCRATCH_MODULE)
    obj = directory / (name + '.o')
    result = run('gcc', *MODULE_FLAGS, '-Ikernel/include', '-c', str(source), '-o', str(obj))
    assert result.returncode == 0, result.stdout + result.stderr
    linked = directory / (name + '.linked.o')
    result = run('ld', '-r', '-m', 'elf_x86_64', '-o', str(linked), str(obj))
    assert result.returncode == 0, result.stdout + result.stderr
    return linked


def witness(directory, family, object_path):
    """A directory shaped like build/, so --objects can be pointed at an arbitrary witness."""
    (directory / 'gpu').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(object_path, directory / 'gpu' / (family + '.o'))
    return directory


def packed_rxe(report_text):
    return int(report_text.split('rxe=')[1].split()[0])


def test_a_module_that_does_not_match_its_objects_is_refused_with_both_numbers():
    if not (ROOT / 'build/gpu/cirrus.mod').exists():
        print('SKIP: build/gpu/cirrus.mod is missing, so there is no real module to compare against; run `make\'')
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        real_mod = tmp / 'cirrus.mod'
        shutil.copyfile(ROOT / 'build/gpu/cirrus.mod', real_mod)
        # 4524 bytes of text for the real driver, 256 for the throwaway, and the objects directory is what says
        # which of the two a module should have been packed from.
        real = witness(tmp / 'real', 'cirrus', ROOT / 'build/gpu/cirrus.o')
        stub = witness(tmp / 'stub', 'cirrus', compile_stub(tmp, 'stub'))
        stub_mod = tmp / 'cirrus.stub.mod'
        stub_text = run('python3', TOOL, '--input', str(stub / 'gpu' / 'cirrus.o'), '--family', 'cirrus',
                        '--out', str(stub_mod), '--report')
        assert stub_text.returncode == 0, stub_text.stdout + stub_text.stderr
        stub_rxe = packed_rxe(stub_text.stdout)
        real_rxe = packed_rxe(run('python3', TOOL, '--input', str(real / 'gpu' / 'cirrus.o'), '--family',
                                  'cirrus', '--out', str(tmp / 'cirrus.repacked.mod'), '--report').stdout)
        assert stub_rxe < real_rxe, 'the fixture has to be smaller than the driver it stands in for'

        # The stub module, weighed against the real family's object: refused, naming both sizes, the directory
        # the witness came from, and the command that puts the right bytes back.
        result = run('python3', TOOL, '--verify', str(stub_mod), '--objects', str(real))
        assert result.returncode == 1, result.stdout + result.stderr
        text = result.stdout + result.stderr
        assert 'read/execute block is %u bytes' % stub_rxe in text, text
        assert 'the cirrus objects in %s hold ' % real in text, text
        assert 'stale or truncated' in text, text
        assert 'make -B %s' % stub_mod in text, \
            'the advice has to name the file that is wrong: ' + text
        # The same complaint the other way round: a driver-sized module against a throwaway witness.  The packed
        # block is the object's sections plus 16-byte alignment, so a surplus that large is a mismatch too.
        result = run('python3', TOOL, '--verify', str(real_mod), '--objects', str(stub))
        assert result.returncode == 1, 'a %d-byte module and a %d-byte object are a mismatch too' % (
            real_rxe, stub_rxe)
        mismatch = result.stdout + result.stderr
        assert 'alignment slack' in mismatch, mismatch
        assert str(real_mod) in mismatch, mismatch
        witness_rxe = int(re.search(r'block is (\d+) bytes against (\d+) in the objects', mismatch).group(2))
        assert int(re.search(r'block is (\d+) bytes against', mismatch).group(1)) == real_rxe, mismatch
        assert 0 < real_rxe - witness_rxe, 'the audit printed a number that is not the object\'s: ' + mismatch

        # With nothing to compare against, `--verify' reports the module and invents no second failure, because
        # a tree that was not built has no witness.
        empty = tmp / 'empty'
        empty.mkdir()
        result = run('python3', TOOL, '--verify', str(real_mod), '--objects', str(empty))
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'objects=' not in result.stdout, result.stdout
        # And the right module against the right witness is accepted, with the witness printed so a reader can
        # see what was compared rather than taking the OK on faith.
        result = run('python3', TOOL, '--verify', str(real_mod), '--objects', str(real))
        assert result.returncode == 0, result.stdout + result.stderr
        assert 'objects=%d/' % witness_of(real / 'gpu' / 'cirrus.o') in result.stdout, result.stdout


def witness_of(object_path):
    """What an object's read/execute sections hold, measured by binutils rather than by this repo's own ELF
    reader, so the number the audit prints is checked against a second opinion and not against the code that
    printed it."""
    out = run('size', '-A', str(object_path))
    assert out.returncode == 0, out.stdout + out.stderr
    total = 0
    for line in out.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith(('.text', '.rodata')):
            total += int(fields[1])
    return total


def test_the_shipped_tree_passes_its_own_audit():
    """The point of the check: `make' runs it, so a stale module in build/ stops the build instead of riding
    along into dist/scos.img.  This is that same command, run on this tree."""
    if not (ROOT / 'build/gpu').is_dir():
        print('SKIP: build/gpu missing; run `make\' first')
        return
    result = run('python3', TOOL, '--verify')
    assert result.returncode == 0, ('build/gpu holds a module its objects do not account for:\n'
                                    + result.stdout + result.stderr)
    for line in result.stdout.splitlines():
        if line.startswith('OK') and 'cirrus' in line:
            assert 'objects=' in line, \
                'the audit ran without saying what it compared against: ' + line


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for _name, fn in tests:
        fn()
    print('PASS: a packed module that does not weigh what its objects weigh is refused by name, with both '
          'numbers and the rebuild that fixes it')


if __name__ == '__main__':
    main()
