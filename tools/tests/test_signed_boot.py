"""SCos signed boot chain: RSA verifier KATs, manifest completeness, and real
QEMU boots of tampered images that must refuse to load a kernel.

The first test compiles the SAME rsa_verify.c object that is linked into
BOOTX64.EFI (with the kernel's SHA-256) and checks it against signatures made
by tools/scos_sign.py.  The second reads BOOT.SIG back out of the packed FAT32
ESP and checks every listed hash against the packed bytes.  The last two boot
modified copies of the image through the real UEFI stub: one byte changed in
KERNEL.ELF or in the signature must stop the boot before any kernel code runs.
"""
import hashlib
import importlib.util
import json
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))
import scos_sigformat as fmt  # noqa: E402
import scos_sign  # noqa: E402

HOST_CFLAGS = ['-m64', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror']
IMAGE = ROOT / 'build' / 'scos.img'


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert r.returncode == 0, ' '.join(cmd) + '\n' + r.stdout[-3000:] + r.stderr[-3000:]
    return r


def reader():
    spec = importlib.util.spec_from_file_location('scos_verify_stick', ROOT / 'tools/verify-stick.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def load_trust_anchor():
    text = (ROOT / 'boot/uefi/trust_anchor.h').read_text()
    values = [int(v, 16) for v in text.split('{', 1)[1].split('}', 1)[0]
              .replace('\n', '').split(',') if v.strip()]
    assert len(values) == 256
    return bytes(values)


def test_the_rsa_verifier_matches_the_signer():
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    objects = []
    for src, name in ((ROOT / 'boot/uefi/rsa_verify.c', 'rsa'),
                      (ROOT / 'kernel/desktop/sec_core.c', 'sec_core')):
        obj = build / ('signed_boot_host_%s.o' % name)
        run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-Iboot/uefi', '-c',
                                     str(src), '-o', str(obj)])
        objects.append(str(obj))
    host = build / 'signed-boot-host'
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-Iboot/uefi',
                                 'tools/tests/rsa_host.c'] + objects + ['-o', str(host)])

    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        modulus = load_trust_anchor()
        (t / 'n.bin').write_bytes(modulus)

        def verify(message, signature):
            (t / 'm.bin').write_bytes(message)
            (t / 's.bin').write_bytes(signature)
            out = subprocess.run([str(host), str(t / 'm.bin'), str(t / 's.bin'),
                                  str(t / 'n.bin')], cwd=ROOT, capture_output=True, text=True)
            assert out.returncode == 0, out.stderr
            return out.stdout.strip() == '1'

        manifest = (build / 'BOOT.SIG').read_bytes()
        parsed, sig = fmt.parse_sigfile(manifest)
        entries = fmt.parse_manifest(parsed)
        assert [e[0] for e in entries][:4] == list(fmt.MANDATORY)
        assert verify(parsed, sig), 'a genuine RSA-2048 signature did not verify in the EFI object'

        bad = bytearray(parsed)
        bad[60] ^= 1
        assert not verify(bytes(bad), sig), 'a changed manifest still verified'

        badsig = bytearray(sig)
        badsig[3] ^= 0x40
        assert not verify(parsed, bytes(badsig)), 'a changed signature still verified'

        assert not verify(parsed, b'\0' * 256), 'an all-zero signature verified'
        assert not verify(parsed, b'\xff' * 256), 'an out-of-range signature verified'

        other = bytearray(modulus)
        other[0] ^= 1
        (t / 'n.bin').write_bytes(bytes(other))
        assert not verify(parsed, sig), 'a signature verified against the wrong trust anchor'
        (t / 'n.bin').write_bytes(modulus)

        empty_sig = scos_sign.sign(b'')
        assert verify(b'', empty_sig), 'signing an empty message broke PKCS#1 v1.5'
        assert not verify(b'x', empty_sig), 'a signature was accepted for a different message'
    print('PASS: EFI RSA-2048/PKCS#1 v1.5 verifier matches the signer and rejects '
          'manifest, signature, key and message tampering')


def test_the_packed_manifest_covers_every_boot_file():
    image = IMAGE.read_bytes()
    vs = reader()
    sigblob = vs.read_named_file(IMAGE, 'BOOT', 'SIG')
    assert sigblob, 'BOOT.SIG is not in the packed ESP'
    manifest, signature = fmt.parse_sigfile(sigblob)
    entries = fmt.parse_manifest(manifest)
    names = [e[0] for e in entries]
    for required in fmt.MANDATORY:
        assert required in names, '%s is not signed' % required
    modules = [p.name.upper() for p in sorted((ROOT / 'build' / 'gpu').glob('*.mod'))]
    for name in modules:
        assert name in names, '%s is executable but not in the signed manifest' % name
    for name, size, digest in entries:
        stem, ext = name.split('.')
        blob = vs.read_named_file(IMAGE, stem, ext)
        assert blob is not None and len(blob) == size, '%s size differs on disk' % name
        assert hashlib.sha256(blob).digest() == digest, '%s hash differs on disk' % name
    key = json.loads((ROOT / 'keys/boot-signing.json').read_text())
    n = int(key['n'], 16)
    em = pow(int.from_bytes(signature, 'big'), 65537, n).to_bytes(256, 'big')
    assert em == scos_sign.emsa_pkcs1_v15_sha256(manifest), 'packed signature does not verify'
    print('PASS: signed manifest covers %d boot files and its RSA signature verifies' % len(entries))


def _boot_serial(image, seconds=20):
    lines = subprocess.check_output(
        ['python3', 'tools/run_qemu.py', '--image', str(image), '--memory', '128',
         '--dry-run'], cwd=ROOT, text=True).splitlines()
    rundir = Path(lines[0].split(': ', 1)[1])
    cmd = shlex.split(lines[-1])
    log = (rundir / 'host.log').open('wb')
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=log)
    try:
        deadline = time.time() + seconds
        while time.time() < deadline:
            text = (rundir / 'serial.log').read_text(errors='replace') \
                if (rundir / 'serial.log').exists() else ''
            if 'HALTED: boot chain verification failed' in text or 'wm: entering main loop' in text:
                break
            time.sleep(.25)
        return text
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        shutil.rmtree(rundir, ignore_errors=True)


def _tampered_copy(stem, ext, offset, label):
    """Flip one byte inside a named ESP file; return a path to a modified image."""
    original = IMAGE.read_bytes()
    vs = reader()
    blob = vs.read_named_file(IMAGE, stem, ext)
    assert blob and len(blob) > offset + 1
    needle = blob[:256]
    at = original.find(needle)
    assert at >= 0, 'could not locate %s.%s in the image' % (stem, ext)
    copy = bytearray(original)
    copy[at + offset] ^= 1
    path = ROOT / 'build' / ('signed-boot-%s.img' % label)
    path.write_bytes(bytes(copy))
    return path


def test_a_tampered_kernel_is_refused_before_any_kernel_code_runs():
    path = _tampered_copy('KERNEL', 'ELF', 4096, 'kernel')
    try:
        serial = _boot_serial(path)
        assert 'HALTED: boot chain verification failed' in serial, serial[-2000:]
        assert 'wm: entering main loop' not in serial, 'a modified kernel booted'
        assert 'security: sign-in required' not in serial, 'a modified kernel reached the sign-in gate'
    finally:
        path.unlink(missing_ok=True)
    print('PASS: one changed KERNEL.ELF byte makes the UEFI stub refuse the boot chain')


def test_a_forged_signature_is_refused():
    path = _tampered_copy('BOOT', 'SIG', 40, 'signature')
    try:
        serial = _boot_serial(path)
        assert 'HALTED: boot chain verification failed' in serial, serial[-2000:]
        assert 'wm: entering main loop' not in serial, 'an invalid signature booted a kernel'
    finally:
        path.unlink(missing_ok=True)
    print('PASS: one changed BOOT.SIG byte makes the UEFI stub refuse the boot chain')


def test_the_stub_and_packer_are_wired():
    main = (ROOT / 'boot/uefi/main.c').read_text()
    assert 'verify_boot_chain(root)' in main, 'signed boot verification is not called'
    assert 'boot_rsa_verify' in main, 'the RSA verifier is not linked into the loader'
    assert 'manifest_has(mname)' in main, 'an unsigned GPU module could still be staged'
    disk = (ROOT / 'tools/makedisk.py').read_text()
    assert 'sign_manifest' in disk and "entry('BOOT.SIG'" in disk, 'the packer does not sign'
    mk = (ROOT / 'Makefile').read_text()
    assert 'efi-rsa_verify.o' in mk and 'efi-sec_core.o' in mk, 'the verifier is not linked'
    print('PASS: loader, packer and Makefile keep the signed boot chain wired')


def test():
    test_the_rsa_verifier_matches_the_signer()
    test_the_packed_manifest_covers_every_boot_file()
    test_the_stub_and_packer_are_wired()
    test_a_tampered_kernel_is_refused_before_any_kernel_code_runs()
    test_a_forged_signature_is_refused()


if __name__ == '__main__':
    test()
