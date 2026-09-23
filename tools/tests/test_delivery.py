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
