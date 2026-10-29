#!/usr/bin/env python3
"""The sizing arithmetic and the file-shaped delivery, which is what a Ventoy stick needs.

Why this suite exists: a GB20x with an installed NVIDIA driver has one firmware file that no public archive
carries - `gsp-<version>.bin`, 63571696 bytes - and it does not fit the 64 MiB image this repository tracks.
The answer is to pack a bigger image as a *file* and let Ventoy boot the file, which puts three new things in
the path: a size the caller writes as `256M', a minimum size the packer has to compute and quote when it refuses,
and `tools/flash.sh --ventoy', which packs, copies, and reads the copy back.  Each of those is a place where a
silent off-by-one becomes a stick that boots an older build or a firmware that never arrives, so each is
measured here rather than believed.

    python3 tools/tests/test_ventoy_image.py
"""
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

_spec = importlib.util.spec_from_file_location('scos_makedisk', ROOT / 'tools/makedisk.py')
makedisk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(makedisk)

_fw_spec = importlib.util.spec_from_file_location('scos_gsp_fw_test', ROOT / 'tools/tests/test_gsp_firmware.py')
gsp_fw = importlib.util.module_from_spec(_fw_spec)
_fw_spec.loader.exec_module(gsp_fw)

RM_IMAGE_BYTES = 63571696          # gsp-570.144.bin, measured on the machine this project is developed against


def check(what, ok):
    if not ok:
        raise AssertionError(what)


def size_parsing_is_the_number_a_person_meant():
    for text, want in (('256M', 256 * 1024 ** 2), ('1g', 1024 ** 3), ('67108864', 67108864),
                       ('128 MB', 128 * 1024 ** 2), ('4K', 4096)):
        check('%s means %d' % (text, want), makedisk.size_bytes(text) == want)
    for bad in ('', 'M', '128Ki', '0x1000', '-1M'):
        try:
            makedisk.size_bytes(bad)
        except ValueError:
            continue
        raise AssertionError('%r was accepted as a size, and a wrong size here is a truncated delivery' % bad)


def the_quoted_minimum_is_the_size_that_fits():
    """`--min-size' is the number a refusal hands over, so it has to be a size the packer accepts and one the
    packer would need anyway: a minimum with slack in it is a lie of the useful kind only if the smaller value
    genuinely fails, which is the second half of this check."""
    with tempfile.TemporaryDirectory() as tmp:
        fw = Path(tmp, 'gsp')
        fw.mkdir()
        blob, fmc_name = fmc_fixture()
        (fw / fmc_name).write_bytes(blob)
        (fw / 'gsp-570.144.bin').write_bytes(rm_fixture(fw))
        want = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(fw),
                               '--min-size'], cwd=ROOT, capture_output=True, text=True)
        check('--min-size exited %d: %s' % (want.returncode, want.stderr), want.returncode == 0)
        smallest = int(want.stdout.split()[0])
        check('a minimum below the tracked 64 MiB is not a minimum', smallest >= 67108864)
        check('a minimum that is not a whole MiB is not a number to give a person', smallest % 1048576 == 0)
        check('the resource-manager image alone is 63571696 B, so 64 MiB cannot be enough',
              smallest > RM_IMAGE_BYTES)
        out = Path(tmp, 'just-fits.img')
        fits = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(fw),
                               '--size', str(smallest), '--out', str(out)], cwd=ROOT, capture_output=True, text=True)
        check('the size the packer quoted does not fit it: %s' % (fits.stdout + fits.stderr),
              fits.returncode == 0)
        small = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(fw),
                               '--size', str(smallest - 1048576), '--out', str(Path(tmp, 'nope.img'))],
                              cwd=ROOT, capture_output=True, text=True)
        check('one MiB smaller than the quoted minimum packed anyway, so the minimum was padded',
              small.returncode != 0)
        quoted = re.search(r'pass --size (\S+?),', small.stderr)
        check('the refusal is not one readable sentence naming a size to try: %r' % small.stderr,
              quoted is not None and 'Traceback' not in small.stderr)
        retry = Path(tmp, 'quoted.img')
        again = subprocess.run([sys.executable, 'tools/makedisk.py', 'build', '--firmware', str(fw),
                               '--size', quoted.group(1), '--out', str(retry)], cwd=ROOT,
                              capture_output=True, text=True)
        check('the size the refusal quoted does not actually fit it: %s' % (again.stdout + again.stderr),
              again.returncode == 0)


def fmc_fixture():
    """The FMC container this build stages - the file the vendor ships, under the name the build gives it.  A
    hand-built one would be a fixture only this suite believes in, and the point of these checks is that
    flash.sh refuses a directory it cannot vouch for: the gate has to be passed by something real."""
    staged = ROOT / 'build/gsp/fmc-615.71.10.bin'
    if staged.is_file():
        return staged.read_bytes(), 'fmc-615.71.10.bin'
    return gsp_fw.build_fmc_image(hash_bytes=48, publickey_bytes=97, signature_bytes=96), 'fmc-570.144.bin'


def rm_fixture(directory):
    """An ELF64 image with a `.fwimage' payload and a `.fwsignature_gb20x' beside it, at the length the real
    resource-manager image has: what gspcheck and the packer must agree about is a file of that size, and the
    container's own shape is what the kernel's walk reads."""
    blob = gsp_fw.build_container(payload_bytes=RM_IMAGE_BYTES - 4096)
    path = Path(directory, 'gsp-570.144.bin')
    path.write_bytes(blob)
    return blob


def flash_ventoy_writes_a_file_and_reads_it_back():
    with tempfile.TemporaryDirectory() as tmp:
        fw, stick = Path(tmp, 'driver'), Path(tmp, 'Ventoy')
        fw.mkdir()
        stick.mkdir()
        blob, fmc_name = fmc_fixture()
        (fw / fmc_name).write_bytes(blob)
        rm = rm_fixture(fw)
        r = subprocess.run(['bash', 'tools/flash.sh', '--ventoy', str(stick), '--firmware', str(fw)],
                           cwd=ROOT, capture_output=True, text=True)
        check('flash.sh --ventoy failed (%d):\n%s%s' % (r.returncode, r.stdout, r.stderr), r.returncode == 0)
        files = sorted(stick.iterdir())
        check('the stick holds %r, exactly one image and no checksum file to rot' % [f.name for f in files],
              len(files) == 1 and files[0].name.startswith('SCOS-') and files[0].suffix == '.img')
        image = files[0]
        check('the image is smaller than the firmware it has to carry',
              image.stat().st_size > len(rm))
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        check('flash.sh did not print the sha256 of the file it left behind: %r' % r.stdout[-400:],
              re.search(r'^  sha256 ([0-9a-f]{64})$', r.stdout, re.M) is not None
              and re.search(r'^  sha256 ([0-9a-f]{64})$', r.stdout, re.M).group(1) == digest)
        got = subprocess.run([sys.executable, 'tools/verify-stick.py', str(image)], cwd=ROOT,
                             capture_output=True, text=True)
        check('the record inside the image cannot be read back: %s' % got.stderr, got.returncode == 0)
        listing = [l for l in got.stdout.splitlines() if 'files on' in l][0]
        check('the image does not hold both firmware files: %s' % listing,
              'GSP.BIN (%d B)' % len(rm) in listing and 'GSPFMC.BIN' in listing)
        check('the record inside the image quotes a different hash for itself',
              'image sha256  : %s' % digest in got.stdout, )
        read = makedisk.esp_reader().read_named_file(image, 'GSP', 'BIN')
        check('GSP.BIN inside the ESP is not the file that was staged', read == rm)


def flash_never_destroys_what_it_was_not_told_to():
    with tempfile.TemporaryDirectory() as tmp:
        stick = Path(tmp, 'Ventoy')
        stick.mkdir()
        both = subprocess.run(['bash', 'tools/flash.sh', '--ventoy', str(stick), '--device', '/dev/null'],
                              cwd=ROOT, capture_output=True, text=True)
        check('one command was allowed to both write a file and overwrite a disk',
              both.returncode == 2 and 'alternatives' in both.stderr)
        nowhere = subprocess.run(['bash', 'tools/flash.sh', '--ventoy', str(stick / 'nope')], cwd=ROOT,
                                 capture_output=True, text=True)
        check('a target that is not a directory and no mounted Ventoy stick is a different refusal: %s'
              % nowhere.stderr, nowhere.returncode == 2 and 'mounted Ventoy' in nowhere.stderr)
        # An existing image is only replaced on purpose, and the flag that says so is not the one that says
        # "erase this disk": two different risks, two different flags.
        commit = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=ROOT,
                                capture_output=True, text=True).stdout.strip()
        target = stick / ('SCOS-%s.img' % commit)
        target.write_bytes(b'not an image')
        again = subprocess.run(['bash', 'tools/flash.sh', '--ventoy', str(stick), '--firmware', 'none',
                                '--image', str(ROOT / 'dist/scos.img')], cwd=ROOT,
                               capture_output=True, text=True)
        check('a file was overwritten without --force: %s' % (again.stdout + again.stderr),
              again.returncode == 2 and '--force' in again.stderr)
        check('the refusal wrote anyway', target.read_bytes() == b'not an image')
        forced = subprocess.run(['bash', 'tools/flash.sh', '--ventoy', str(stick), '--firmware', 'none',
                                 '--image', str(ROOT / 'dist/scos.img'), '--force'], cwd=ROOT,
                                capture_output=True, text=True)
        check('copy mode could not put the tracked image on the stick: %s' % (forced.stdout + forced.stderr),
              forced.returncode == 0)
        check('the file that --force replaced is not the image it was told to copy',
              target.stat().st_size == (ROOT / 'dist/scos.img').stat().st_size
              and hashlib.sha256(target.read_bytes()).hexdigest()
              == hashlib.sha256((ROOT / 'dist/scos.img').read_bytes()).hexdigest())
        check('the copied image holds no readable record of what it is',
              subprocess.run([sys.executable, 'tools/verify-stick.py', str(target)], cwd=ROOT,
                             capture_output=True, text=True).returncode == 0)


def a_stick_that_boots_files_is_not_a_disk_to_overwrite():
    """The device mode's new refusal.  It cannot be exercised against a real stick here, so the guard is
    checked where it is decided: the label scan and the flag that overrides it."""
    text = (ROOT / 'tools/flash.sh').read_text()
    check('the raw path no longer refuses a Ventoy disk', 'has a Ventoy filesystem on it' in text)
    check('the override flag is named in the message that asks for it', '--force' in text)
    check('the file mode is the one offered first, in the help text',
          text.index('--ventoy') < text.index('--device'))


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith(('size_', 'the_', 'flash_', 'a_'))
             and callable(v)]
    for test in tests:
        test()
    print('PASS: the size a stick needs, the refusal that names it, and an image file Ventoy can boot - '
          '%d scenario(s)' % len(tests))


if __name__ == '__main__':
    main()
