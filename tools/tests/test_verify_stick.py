#!/usr/bin/env python3
"""The stick-verifying tool, tested against the image this repository ships.

It exists because a report from a machine that booted the previous image is indistinguishable, in prose,
from a report from a machine that booted the current one - the only difference is which sentences the panel
can still print.  So the check has to be against the bytes on the stick.  These tests make sure the tool
reads them rather than restating what the repository already believes: one case patches a scratch copy of
the image and requires the tool to notice.
"""
import re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / 'dist/scos.img'
RECORD = ROOT / 'dist/scos.img.build.json'
SCRATCH = ROOT / 'build/scratch-stick'


def run(*args):
    return subprocess.run([sys.executable, 'tools/verify-stick.py'] + list(args), cwd=ROOT,
                          capture_output=True, text=True)


def test_the_record_on_the_shipped_image_matches_the_repository():
    if not IMAGE.is_file():
        print('skip: dist/scos.img is not built; run make dist')
        return
    result = run(str(IMAGE.relative_to(ROOT)), '--against', str(RECORD.relative_to(ROOT)))
    print(result.stdout.strip())
    assert result.returncode == 0, result.stdout + result.stderr
    text = result.stdout
    want = re.search(r'  commit\s+: (\S+)', text)
    assert want, text
    assert want.group(1) in RECORD.read_text(), 'the commit on the disk is not the one beside it'
    for name in ('BUILD.TXT', 'NVIDIA.MOD', 'KERNEL.ELF'):
        assert name in text, 'the listing lost ' + name
    assert 'MISMATCH' not in text, text


def test_a_stale_image_is_reported_as_stale():
    """Patch the packed record and require the tool to disagree with the repository copy."""
    if not IMAGE.is_file():
        print('skip: dist/scos.img is not built; run make dist')
        return
    SCRATCH.mkdir(parents=True, exist_ok=True)
    copy = SCRATCH / 'stale.img'
    shutil.copyfile(IMAGE, copy)
    blob = bytearray(copy.read_bytes())
    marker = re.search(rb'  commit      : (.{12})', bytes(blob))
    assert marker, 'no commit line in the packed build record'
    blob[marker.start(1):marker.end(1)] = b'000000000000'
    copy.write_bytes(bytes(blob))
    result = run(str(copy.relative_to(ROOT)), '--against', str(RECORD.relative_to(ROOT)))
    print(result.stdout.strip()[-600:])
    assert result.returncode == 1, 'a doctored record was accepted: %s' % result.stdout
    mismatch = result.stdout.split('MISMATCH:')[-1]
    assert 'commit' in mismatch and '000000000000' in mismatch, mismatch
    assert 'the repository says' in mismatch, mismatch


def test_the_tool_refuses_a_device_it_cannot_read():
    SCRATCH.mkdir(parents=True, exist_ok=True)
    junk = SCRATCH / 'not-a-stick.bin'
    junk.write_bytes(bytes(8 * 1024 * 1024))
    result = run(str(junk.relative_to(ROOT)))
    assert result.returncode == 2, result.stdout + result.stderr
    assert 'no FAT32 boot sector' in result.stderr, result.stderr
    print('PASS: an image with no ESP is refused in as many words')


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_') and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
        except AssertionError as exc:
            failed += 1
            print('FAIL: %s: %s' % (t.__name__, str(exc)[-800:]))
    print(('FAIL: %d of %d' % (failed, len(tests))) if failed else
          ('PASS: stick verification, %d test(s)' % len(tests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
