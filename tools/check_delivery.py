#!/usr/bin/env python3
"""Check that dist/scos.img, its checksum and its build record are one delivery rather than three files.

Why this exists: the record beside the image names the commit the packed bytes came from.  When the image is
not refreshed after a commit that changes what gets packed, the record and the tree disagree, and the only
person who can notice is whoever boots it - which is how a branch can end up carrying a 64 MiB artifact
whose provenance file describes something else.  `make dist` runs this at the end of its own recipe, so a
delivery that has drifted fails the build that made it.

What it checks, in the order the failure modes matter:

  1. the three files exist, the image is 67108864 bytes, and its sha256 is what dist/scos.img.sha256 says;
  2. the record's tree state is `clean` - an experiment is not a delivery;
  3. the record names a commit that is reachable from HEAD, so it describes this branch;
  4. the bytes *inside* the image match the record: KERNEL.ELF and every .MOD are read back out of the
     packed FAT32 ESP and hashed, so a record refreshed without repacking (or the reverse) is caught;
  5. nothing that lands on the image changed between the record's commit and HEAD.  Docs, tests and
     host-side tools may move freely; kernel, boot stub, driver modules, the packer and the flags may not.

The stamp and HEAD are deliberately allowed to differ by one commit: a delivery is made by committing the
code, running make dist so the record names that commit, then committing the artifacts.  What is not allowed
is a record that names a commit whose *packed* inputs have since moved.
"""
import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'dist/scos.img'
RECORD = 'dist/scos.img.build.json'
CHECKSUM = 'dist/scos.img.sha256'
IMAGE_BYTES = 67108864
# Everything that ends up packed into the image.  A change here between the record's commit and HEAD means
# the artifact on the branch is not the artifact this source would produce.
IMAGE_INPUTS = ('kernel/', 'boot/', 'drivers/', 'tools/makedisk.py', 'tools/research/gen_gpu_tables.py',
                'Makefile')


def git(*args, **kwargs):
    return subprocess.run(['git'] + list(args), cwd=ROOT, capture_output=True, text=True, **kwargs)


def _reader():
    """tools/verify-stick.py holds the ESP reader; the hyphen is why it is loaded by path."""
    spec = importlib.util.spec_from_file_location('scos_verify_stick', ROOT / 'tools/verify-stick.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(root=ROOT):
    """Return a list of problems; empty means the delivery is self-consistent."""
    root = Path(root)
    problems = []
    image, record, checksum = (root / IMAGE, root / RECORD, root / CHECKSUM)
    for path in (image, record, checksum):
        if not path.is_file():
            problems.append('%s is missing: a delivery is the image, its checksum and its record together'
                            % path.relative_to(root))
    if problems:
        return problems

    blob = image.read_bytes()
    if len(blob) != IMAGE_BYTES:
        problems.append('%s is %d bytes, not %d' % (IMAGE, len(blob), IMAGE_BYTES))
    digest = hashlib.sha256(blob).hexdigest()
    wanted = (checksum.read_text().strip().split() or [''])[0]
    if digest != wanted:
        problems.append('checksum mismatch: the file holds %s, %s says %s' % (digest, CHECKSUM, wanted))

    try:
        meta = json.loads(record.read_text())
    except ValueError as exc:
        return problems + ['%s is not readable JSON: %s' % (RECORD, exc)]
    if meta.get('tree') != 'clean':
        problems.append('the record says tree %r: an image built from an uncommitted tree is an experiment, '
                        'not a delivery' % meta.get('tree'))
    commit = meta.get('commit', '')
    if len(commit) < 12 or git('cat-file', '-e', commit + '^{commit}').returncode != 0:
        problems.append('the record names commit %r, which this repository does not contain' % commit)
    elif git('merge-base', '--is-ancestor', commit, 'HEAD').returncode != 0:
        problems.append('the record\'s commit %s is not an ancestor of HEAD: the delivery does not belong '
                        'to this branch' % commit[:12])

    # Read the packed bytes back out of the image rather than trusting either file's word for it.
    reader = _reader()
    try:
        kernel = reader.read_named_file(image, 'KERNEL', 'ELF')
        packed = {}
        for module in meta.get('modules', []):
            stem = module['name'].rsplit('.', 1)[0][:8].upper()
            ext = module['name'].rsplit('.', 1)[-1].upper()
            packed[module['name']] = reader.read_named_file(image, stem, ext)
    except ValueError as exc:
        return problems + ['the image holds no readable ESP to check against: %s' % exc]
    if kernel is None:
        problems.append('KERNEL.ELF is not in the image the record describes')
    elif meta.get('kernel_bytes') != len(kernel):
        problems.append('the record says the kernel is %r B; the image holds %d B'
                        % (meta.get('kernel_bytes'), len(kernel)))
    elif hashlib.sha256(kernel).hexdigest() != meta.get('kernel_sha256'):
        problems.append('KERNEL.ELF inside the image is not the hash the record carries')
    for name, blob_for_name in packed.items():
        want = next(m for m in meta.get('modules', []) if m['name'] == name)
        if blob_for_name is None:
            problems.append('%s is listed by the record but absent from the image' % name)
        elif len(blob_for_name) != want.get('bytes'):
            problems.append('%s: the image holds %d B, the record says %r B'
                            % (name, len(blob_for_name), want.get('bytes')))
        elif hashlib.sha256(blob_for_name).hexdigest() != want.get('sha256'):
            problems.append('%s inside the image does not hash to the record value' % name)

    if commit and git('cat-file', '-e', commit + '^{commit}').returncode == 0:
        moved = git('diff', '--name-only', commit + '..HEAD').stdout.split()
        changed = [p for p in moved if p.startswith(IMAGE_INPUTS)]
        if changed:
            problems.append('the image predates commits that change what gets packed (%s); run make dist '
                            'and commit the refreshed image, record and checksum together'
                            % ', '.join(sorted(set(changed))[:6]))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', type=Path, default=ROOT, help='repository root, or a scratch copy of one')
    ap.add_argument('--report-only', action='store_true',
                    help='print what is wrong and exit 0 anyway; for a suite checking what is detected')
    args = ap.parse_args()
    problems = check(args.root)
    if not problems:
        meta = json.loads((args.root / RECORD).read_text())
        print('PASS: dist/scos.img, its checksum and its record are one delivery from %s (kernel %d B, %d '
              'module file(s))' % (meta['commit'], meta['kernel_bytes'], len(meta['modules'])))
        return 0
    print('FAIL: the delivery is not self-consistent:')
    for problem in problems:
        print('  - ' + problem)
    return 1


if __name__ == '__main__':
    sys.exit(main())
