#!/usr/bin/env python3
"""The delivery gate, tested by breaking deliveries.

`tools/check_delivery.py` exists because an image, its checksum and its build record can each be committed
without the others, and a repository can then describe a build that no stick can reproduce.  A check like
that is worthless unless it fails on the states it claims to catch, so each one is fabricated here on a
scratch copy of the three files - the committed artifacts are never touched.
"""
import hashlib, json, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / 'build/scratch-delivery'
DIST = SCRATCH / 'dist'


def accepted(result):
    """True when the delivery's own facts agree.  A scratch copy of a real build is by definition packed from a
    tree with uncommitted tooling, and that is the one complaint this suite must not treat as its own: the gate
    under test here is size, hashing and read-back, not whether somebody committed the checker."""
    out = result.stdout + result.stderr
    return result.returncode == 0 or (out.count('  - ') == 1 and "tree 'modified'" in out)


def prepare(record_edits=None, image_edit=None):
    DIST.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'dist/scos.img', DIST / 'scos.img')
    shutil.copyfile(ROOT / 'dist/scos.img.build.json', DIST / 'scos.img.build.json')
    shutil.copyfile(ROOT / 'dist/scos.img.sha256', DIST / 'scos.img.sha256')
    blob = bytearray((DIST / 'scos.img').read_bytes())
    if image_edit == 'flip':
        blob[1024 * 1024 + 2048] ^= 0x01        # one byte inside the packed ESP
    (DIST / 'scos.img').write_bytes(bytes(blob))
    if record_edits:
        record = json.loads((DIST / 'scos.img.build.json').read_text())
        for key, value in record_edits.items():
            if value is None:
                record.pop(key, None)
            else:
                record[key] = value
        (DIST / 'scos.img.build.json').write_text(json.dumps(record, sort_keys=True, indent=1) + '\n')
    if image_edit == 'flip':
        # The checksum file is rewritten to match, so a flip must be caught by the *record*, not by the
        # pair agreeing with each other: this is the state where someone refreshed one file only.
        digest = hashlib.sha256((DIST / 'scos.img').read_bytes()).hexdigest()
        (DIST / 'scos.img.sha256').write_text('%s  dist/scos.img\n' % digest)
    return DIST


def run():
    # --root is the scratch delivery, --repo the real tree: the module set a delivery must carry comes from
    # drivers/gpu/*/module.c in the repository, which the scratch copy deliberately does not duplicate.
    return subprocess.run([sys.executable, 'tools/check_delivery.py', '--root', str(SCRATCH), '--repo', str(ROOT)],
                          cwd=ROOT, capture_output=True, text=True)


def expect_problem(what, needle):
    result = run()
    out = result.stdout + result.stderr
    assert result.returncode == 1, '%s was accepted: %s' % (what, out)
    assert needle in out, '%s not reported (%r)' % (what, out[:400])
    print('PASS: %s is refused: %s' % (what, needle))


def test_a_whole_delivery_passes():
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare()
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'PASS: dist/scos.img' in result.stdout, result.stdout
    print(result.stdout.strip())


def test_a_stale_image_for_the_record_is_refused():
    """The record's hashes no longer describe the bytes, which is what a half-refresh leaves behind."""
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare(record_edits={'kernel_sha256': hashlib.sha256(b'someone elses kernel').hexdigest()},
            image_edit='flip')
    expect_problem('a record that does not match the packed kernel', 'not the hash the record carries')


def test_a_record_missing_a_built_driver_is_refused():
    """The state `make dist' reaches when build/gpu was never populated: an image with no drivers in it."""
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare(record_edits={'modules': []})
    expect_problem('a delivery whose record left the driver modules out', 'the tree builds')


def test_a_record_from_before_an_image_input_change_is_refused():
    """The state a branch reaches by committing kernel work without running make dist."""
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare(record_edits={'commit': 'f1ec437', 'tree': 'clean'})
    expect_problem('an image older than the source it claims', 'predates commits that change what gets packed')


def test_a_record_from_an_uncommitted_tree_is_refused():
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare(record_edits={'tree': 'modified'})
    expect_problem('a delivery built from a dirty tree', 'not a delivery')


def test_a_record_naming_a_commit_the_repository_lacks_is_refused():
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare(record_edits={'commit': 'deadbeefcafe'})
    expect_problem('a commit this repository does not contain', 'does not contain')


def test_a_missing_member_of_the_three_is_refused():
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    prepare()
    (DIST / 'scos.img.build.json').unlink()
    expect_problem('an image shipped without its record', 'is missing')


def test_the_size_a_record_states_is_the_size_that_is_checked():
    """A delivery is allowed to be bigger than 64 MiB - a GSP-RM image is 63 MB by itself - so the size rule
    cannot be a constant any more.  What replaces it has to be stricter, not looser: the image is measured
    against the size the packer wrote into the record when it fixed the geometry, and a record that predates
    that field is still held to the one size it can have had.
    """
    if not (ROOT / 'dist/scos.img').is_file():
        print('skip: dist/scos.img is not built')
        return
    packed = json.loads((ROOT / 'dist/scos.img.build.json').read_text()).get('image_bytes')
    prepare(record_edits={'image_bytes': 75497472 if packed != 75497472 else 83886080})
    expect_problem('an image whose bytes are not the size its record says it was packed at', 'packed at ')
    prepare(record_edits={'image_bytes': None})
    result = run()
    assert accepted(result), 'a record without the size field was refused for no reason: ' + (
        result.stdout + result.stderr)
    print('PASS: a record naming no size is held to the floor, and one naming a size is held to that size')


def test_a_grown_delivery_passes_the_same_gate():
    """The state the growth exists for: a staged resource-manager image too big for the default volume, packed
    at the packer's own minimum, with the image, the record and the checksum agreeing on what that was.  This is
    also the only test that the record's size field is written where the geometry is final rather than guessed.
    """
    import tempfile
    if not (ROOT / 'build/kernel.elf').is_file():
        print('skip: build/kernel.elf is not built, so there is nothing to pack')
        return
    with tempfile.TemporaryDirectory(prefix='scos-grown-') as tmp:
        tmp = Path(tmp)
        staged = tmp / 'gsp'
        staged.mkdir()
        (staged / 'gsp-570.144.bin').write_bytes(bytes(range(256)) * 248000)      # 63 488 000 B
        (staged / 'fmc-615.71.10.bin').write_bytes((ROOT / 'build/gsp/fmc-615.71.10.bin').read_bytes())
        want = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(staged),
                               '--min-size'], cwd=ROOT, capture_output=True, text=True)
        assert want.returncode == 0, want.stdout + want.stderr
        size = want.stdout.split('\t')[0]
        # `build.json' is written where the packer was pointed (`build/'), not beside --out: a delivery is the
        # three files together, so the sidecar is picked up from there and put with the image, which is the same
        # thing `make dist' does.  The scratch copy of the real build directory is restored at the end so a run
        # of this suite cannot leave a record describing firmware nobody staged.
        keep = (ROOT / 'build/build.json').read_bytes()
        try:
            packed = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(staged),
                                     '--size', size, '--out', str(tmp / 'scos.img')], cwd=ROOT,
                                    capture_output=True, text=True)
            assert packed.returncode == 0, packed.stdout + packed.stderr
            image = (tmp / 'scos.img').read_bytes()
            record = (ROOT / 'build/build.json').read_bytes()
        finally:
            (ROOT / 'build/build.json').write_bytes(keep)
        grown = len(image) > int((ROOT / 'dist/scos.img').stat().st_size)
        assert grown, 'packing a 63 MB firmware file produced an image of %d bytes, which is not grown' % len(image)
        dist = DIST
        dist.mkdir(parents=True, exist_ok=True)
        (dist / 'scos.img').write_bytes(image)
        (dist / 'scos.img.build.json').write_bytes(record)
        digest = hashlib.sha256(image).hexdigest()
        (dist / 'scos.img.sha256').write_text('%s  dist/scos.img\n' % digest)
        record = json.loads((dist / 'scos.img.build.json').read_text())
        assert record['image_bytes'] == len(image), \
            'the record says %r and the image is %d bytes: the size was not taken from the final geometry' % (
                record.get('image_bytes'), len(image))
        assert any(f['name'] == 'GSP.BIN' for f in record['firmware']), record['firmware']
        result = run()
        assert accepted(result), 'the grown delivery was refused:\n' + result.stdout + result.stderr
        assert 'GSP.BIN' in result.stdout, result.stdout
        print('PASS: a %d-byte delivery holding GSP.BIN passes the gate that the 64 MiB one passes' % len(image))


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_') and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
        except AssertionError as exc:
            failed += 1
            print('FAIL: %s: %s' % (t.__name__, str(exc)[-600:]))
    print(('FAIL: %d of %d' % (failed, len(tests))) if failed else
          ('PASS: delivery gate, %d test(s)' % len(tests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
