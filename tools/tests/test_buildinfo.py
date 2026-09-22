"""What build this image is, and whether the OS, the disk and the repository agree about it.

The reason this suite exists is an incident that wasted a whole exchange: a physical panel was reported
as "the features are not implemented at all" from a USB stick built out of an uncommitted workspace state
whose GPU naming table held 1298 rows while every committed header holds 1296.  Nothing in the running
system could say which commit it came from, so the code under discussion and the machine being judged
were two different builds and neither side could prove it.

So provenance is now part of the product.  tools/buildinfo.py writes build/scosbuild.h from the
repository; kernel/desktop/buildinfo.c is the kernel's only reader of it and is recompiled on every
make, so the stamp cannot lag a commit; `version`, `graphics`, the About panel, the TTY and the serial
log all quote it; tools/makedisk.py refuses to pack a kernel whose compiled-in stamp is not in the
binary; and \\SCOS\\BUILD.TXT on the ESP lets anyone holding the stick check it from another OS.
"""
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu_support import Guest, RESULTS   # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'build' / 'scosbuild.h'
FIELDS = ('COMMIT', 'BRANCH', 'TREE', 'DATE')


def stamp_of(path):
    """The four strings the compiled kernel was told about, read out of the generated header."""
    found = dict(re.findall(r'#define SCOS_BUILD_(\w+) "([^"]*)"', path.read_text()))
    missing = [f for f in FIELDS if f not in found]
    assert not missing, '%s has no build stamp for %s' % (path, ', '.join(missing))
    return found


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def test_the_stamp_describes_a_real_commit_of_this_repository():
    """A stamp is only useful if it points at something a reader can check out."""
    v = stamp_of(HEADER)
    assert re.fullmatch(r'[0-9a-f]{12}|nogit', v['COMMIT']), 'commit field is not a 12-hex id: %r' % v['COMMIT']
    assert v['TREE'] in ('clean', 'modified'), v['TREE']
    assert re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ', v['DATE']), v['DATE']
    if v['COMMIT'] != 'nogit':
        # git cat-file, not git log: the stamp may name a commit that a later rebuild has moved past,
        # and what must hold either way is that the id exists in this repository.
        subprocess.run(['git', 'cat-file', '-e', v['COMMIT'] + '^{commit}'], cwd=ROOT, check=True)
    print('stamp: %s on %s (%s) at %s' % (v['COMMIT'], v['BRANCH'], v['TREE'], v['DATE']))


def test_the_disk_record_matches_the_bytes_it_was_generated_from():
    """build.json and BUILD.TXT are computed from the packed files, not from what the build hoped for."""
    record = json.loads((ROOT / 'build' / 'build.json').read_text())
    header = stamp_of(HEADER)
    keys = {'COMMIT': 'commit', 'BRANCH': 'branch', 'TREE': 'tree', 'DATE': 'built'}
    for field, key in keys.items():
        assert record[key] == header[field], (field, record[key], header[field])
    assert record['kernel_bytes'] == (ROOT / 'build' / 'kernel.elf').stat().st_size
    assert record['kernel_sha256'] == sha256(ROOT / 'build' / 'kernel.elf'), 'stale record: rebuild'
    modules = {m['name']: m for m in record['modules']}
    on_disk = {p.name: p for p in (ROOT / 'build' / 'gpu').glob('*.mod')}
    assert set(modules) == {n.upper() for n in on_disk}, (sorted(modules), sorted(on_disk))
    for name, path in on_disk.items():
        assert modules[name.upper()]['bytes'] == path.stat().st_size, name
        assert modules[name.upper()]['sha256'] == sha256(path), name
    text = (ROOT / 'build' / 'BUILD.TXT').read_text()
    for needle in (header['COMMIT'], header['DATE'], record['kernel_sha256']):
        assert needle in text, 'BUILD.TXT does not mention %s' % needle
    if header['TREE'] != 'clean':
        assert 'uncommitted' in text, 'a modified build must warn about itself on the disk'


def test_the_image_carries_the_record_a_third_party_can_read():
    """The whole point of \\SCOS\\BUILD.TXT: check a stick without booting it, from any OS."""
    image = (ROOT / 'build' / 'scos.img').read_bytes()
    assert b'BUILD   TXT' in image, 'no BUILD.TXT directory entry on the ESP'
    header = stamp_of(HEADER)
    assert ('commit      : %s' % header['COMMIT']).encode() in image
    assert ('built (UTC) : %s' % header['DATE']).encode() in image
    # The kernel is packed verbatim, so its compiled-in stamp is findable in the image too: the disk
    # and the binary it boots cannot have been produced from different trees without being caught.
    assert header['DATE'].encode() in image


def test_makedisk_refuses_a_kernel_that_misreports_itself():
    """The gate: if the ELF does not contain the stamp the record claims, the image is not packed."""
    import makedisk
    header = stamp_of(HEADER)
    elf = (ROOT / 'build' / 'kernel.elf').read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / 'scosbuild.h'
        good.write_text(HEADER.read_text())
        assert makedisk.provenance(tmp, elf) == header        # honest: returns the values
        bad = Path(tmp) / 'bogus'
        bad.mkdir()
        bad.joinpath('scosbuild.h').write_text(
            re.sub(r'#define SCOS_BUILD_COMMIT "[^"]*"', '#define SCOS_BUILD_COMMIT "ffffffffffffff"',
                   HEADER.read_text()))
        try:
            makedisk.provenance(str(bad), elf)
        except SystemExit as exc:
            assert 'refusing to pack' in str(exc), str(exc)
        else:
            raise AssertionError('makedisk packed a kernel whose stamp did not match its record')
        missing = Path(tmp) / 'nostamp'
        missing.mkdir()
        try:
            makedisk.provenance(str(missing), elf)
        except SystemExit as exc:
            assert 'missing' in str(exc), str(exc)
        else:
            raise AssertionError('makedisk packed an image with no build stamp to check against')


def test_the_running_kernel_answers_with_the_same_stamp():
    """`version`, `graphics`, the About panel and the log all read these two functions."""
    if not (ROOT / '.tools' / 'qemu' / 'bin' / 'qemu-system-x86_64').exists():
        raise AssertionError('QEMU bundle missing; run tools/setup_qemu.py before this suite')
    header = stamp_of(HEADER)
    want = '%s %s, %s' % (header['COMMIT'], header['TREE'], header['DATE'])
    with Guest('buildinfo') as g:
        g.run(1.0)
        buf = g.scratch
        g.call('scos_build_stamp', buf, 96)
        got = g.string(buf, 96)
        assert got == want, 'the kernel says %r, the build stamp says %r' % (got, want)
        assert g.call('scos_build_modified') == (1 if header['TREE'] != 'clean' else 0)

        g.call('graphics_report', buf, 65536)
        report = g.string(buf, 65536)
        line = next((l for l in report.splitlines() if l.startswith('Build:')), None)
        assert line, 'graphics does not report the build at all:\n%s' % report[:400]
        assert want in line, (line, want)
        if header['TREE'] != 'clean':
            assert 'UNCOMMITTED' in line, 'a modified build must not look like a committed one: %r' % line

        # The boot log carries it before any desktop exists, so a machine that never reaches the
        # window manager is still identifiable.
        serial = g.serial()
        assert 'build: %s branch %s' % (want, header['BRANCH']) in serial, serial[:400]
        print('   guest: %s' % got)


def test():
    RESULTS.mkdir(parents=True, exist_ok=True)
    if not HEADER.exists():
        raise AssertionError('build/scosbuild.h is missing: this suite checks the build that was just '
                            'packed, so run `make` first')
    test_the_stamp_describes_a_real_commit_of_this_repository()
    test_the_disk_record_matches_the_bytes_it_was_generated_from()
    test_the_image_carries_the_record_a_third_party_can_read()
    test_makedisk_refuses_a_kernel_that_misreports_itself()
    test_the_running_kernel_answers_with_the_same_stamp()


if __name__ == '__main__':
    test()
    print('PASS: the OS, the boot disk and the repository name one and the same build, and makedisk '
          'refuses to pack a kernel that would misreport itself')
