"""SCos account system: crypto known-answer tests, policy/codec tests, the
minimal-integration contract, and a booted guest that proves the gate works.

The core is a freestanding C file (kernel/desktop/sec_core.c) with no kernel
dependencies, so it is compiled on the host and checked against FIPS/RFC
vectors.  The second test is a source-level contract: the places that MUST stay
wired are asserted, so a future refactor cannot quietly remove authentication
while the crypto unit tests keep passing.  The third test boots the real image,
signs in through the same production API the secure screen uses, and checks the
behaviours a person relies on.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

HOST_CFLAGS = ['-m64', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror']


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, ' '.join(cmd) + '\n' + result.stdout[-3000:] + result.stderr[-3000:]
    return result


def test_the_crypto_and_policy_core():
    out = ROOT / 'build' / 'security-host'
    out.parent.mkdir(parents=True, exist_ok=True)
    obj = out.parent / 'sec_core_host.o'
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-c',
                                 'kernel/desktop/sec_core.c', '-o', str(obj)])
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include',
                                 'tools/tests/security_host.c', str(obj), '-o', str(out)])
    result = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.startswith(('PASS', 'FAIL', 'TOTAL'))]
    print('\n'.join(lines))
    assert result.returncode == 0, result.stdout[-3000:]
    assert not [l for l in lines if l.startswith('FAIL')], result.stdout
    assert len([l for l in lines if l.startswith('PASS')]) >= 55, \
        'the security core suite shrank: %d checks' % len(lines)
    print('PASS: SHA-256, HMAC, PBKDF2, policy, codec and checksum vectors')


def test_the_gate_and_protection_are_wired():
    """Places that must never be removed while the crypto tests still pass."""
    plat = (ROOT / 'kernel/desktop/platform.c').read_text()
    assert 'security_init(fs_image_available())' in plat, 'accounts are never initialized'
    assert 'security_login_gate()' in plat, 'the desktop starts without authentication'
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    assert 'security_wm_tick()' in wm, 'queued secure flows and auto-lock are unreachable'
    vfs = (ROOT / 'kernel/desktop/vfs.c').read_text()
    assert 'vfs_denied(path)' in vfs and 'security_protected_path' in vfs, \
        'the password database is writable by any app again'
    assert 'security_fs_restore()' in vfs, 'a factory reset silently drops the account DB'
    term = (ROOT / 'kernel/desktop/app_terminal.c').read_text()
    assert 'security_current_user()' in term, 'the terminal prompt claims a fake user again'
    assert 'security_command_allowed(' in term, 'terminal commands bypass authorization'
    tty = (ROOT / 'kernel/desktop/tty.c').read_text()
    assert 'security_reauthenticate(' in tty, 'the maintenance console has no re-auth gate'
    header = (ROOT / 'kernel/include/security.h').read_text()
    for needle in ('sc_pbkdf2', 'sc_password_eval', 'sc_db_verify', 'SC_FLAG_MUSTCHANGE'):
        assert needle in header, '%s disappeared from the account API' % needle
    core = (ROOT / 'kernel/desktop/sec_core.c').read_text()
    assert 'trailing separator' in core, 'the empty-history reboot regression was removed'
    print('PASS: boot gate, WM hook, VFS guard, both shells and the codec regression are wired')


def test_a_booted_guest_enforces_accounts():
    from qemu_support import Guest  # noqa: E402

    pw = b'Scos-Test-2026!x7Q'
    with Guest('security') as g:
        d, s = g.debug, g.scratch

        def shell(line):
            d.write(s + 8192, line.encode() + b'\0')
            d.write(s + 16384, b'\0' * 1024)
            handled = g.call('security_shell', s + 8192, s + 16384, 1024)
            return handled, g.string(s + 16384, 1024)

        def allowed(line):
            d.write(s + 8192, line.encode() + b'\0')
            d.write(s + 16384, b'\0' * 256)
            ok = g.call('security_command_allowed', s + 8192, s + 16384, 256)
            return ok, g.string(s + 16384, 256)

        def auth(name, password):
            d.write(s + 8192, name.encode() + b'\0')
            d.write(s + 16384, password + b'\0')
            d.write(s + 24576, b'\0' * 192)
            return g.call('security_api_authenticate', s + 8192, s + 16384, s + 24576, 192)

        serial = g.serial()
        assert 'security: sign-in required' in serial, serial[-2000:]
        assert 'wm: entering main loop' in serial, serial[-2000:]
        assert g.call('security_api_gate_satisfied') == 1
        assert g.call('security_api_user_count') == 1
        assert g.string(g.call('security_current_user'), 64) == 'admin'
        assert g.call('security_is_admin') == 1

        # A wrong password and an unknown user both fail with the same message.
        assert auth('admin', b'wrong') == 0
        assert auth('ghost', b'wrong') == 0
        assert auth('admin', pw) == 1
        assert b'PBKDF2-HMAC-SHA256' in shell('security')[1].encode()

        # Credentials and audit data are not readable, writable or removable.
        assert g.read('system/security/users.db') is None
        assert g.read('system/security/audit.log') is None
        assert allowed('cat system/security/users.db')[0] == 0
        d.write(s + 8192, b'system/security/users.db\0')
        d.write(s + 16384, b'pwned\0')
        assert g.call('vfs_write', s + 8192, s + 16384, 5) == 0
        assert g.call('vfs_delete', s + 8192) == 0

        # Password change, history and weak-password rejection.
        p1, weak = b'Zt7#mQ2!vLp9Xk4$', b'password1234'
        d.write(s + 8192, p1 + b'\0')
        d.write(s + 16384, b'\0' * 192)
        assert g.call('security_api_change_password', 0, 0, s + 8192, 0, s + 16384, 192) == 1
        assert auth('admin', pw) == 0 and auth('admin', p1) == 1
        d.write(s + 8192, weak + b'\0')
        assert g.call('security_api_change_password', 0, 0, s + 8192, 0, s + 16384, 192) == 0

        # Guest accounts are read-only in the shell and at the VFS boundary.
        d.write(s + 8192, b'viewer\0')
        d.write(s + 16384, b'Km4!dV9#pLq2Xz7%' + b'\0')
        assert g.call('security_api_create_user', s + 8192, 2, s + 16384) == 1
        assert auth('viewer', b'Km4!dV9#pLq2Xz7%') == 1
        assert g.call('security_api_force_change') == 1          # admin-created accounts must change
        assert allowed('ls')[0] == 1
        assert allowed('touch x')[0] == 0 and allowed('save')[0] == 0
        d.write(s + 8192, b'guest.txt\0')
        d.write(s + 16384, b'data\0')
        assert g.call('vfs_write', s + 8192, s + 16384, 4) == 0
        assert auth('admin', p1) == 1

        assert 'panic' not in g.serial().lower(), g.serial()[-2000:]
    print('PASS: booted guest authenticates, protects its database, changes passwords, '
          'rejects reuse and enforces read-only guests')


def test_a_persistent_account_survives_a_reboot():
    from qemu_support import Guest  # noqa: E402

    pw = b'Scos-Test-2026!x7Q'
    with Guest('security-ata', ide=True) as g:
        assert g.call('security_api_persistent') == 1
        assert g.call('fs_image_save') == 1
        g.run(0.8)
        g.reboot()
        assert g.call('security_api_user_count') == 1
        g.debug.write(g.scratch, b'admin\0')
        g.debug.write(g.scratch + 64, pw + b'\0')
        g.debug.write(g.scratch + 128, b'\0' * 192)
        assert g.call('security_api_authenticate', g.scratch, g.scratch + 64,
                      g.scratch + 128, 192) == 1, 'the stored verifier did not survive reboot'
        d = g.debug
        d.write(g.scratch + 512, b'security\0')
        d.write(g.scratch + 1024, b'\0' * 1024)
        g.call('security_shell', g.scratch + 512, g.scratch + 1024, 1024)
        assert 'verified database' in g.string(g.scratch + 1024, 1024), \
            'the account database was rebuilt from scratch after reboot'
    print('PASS: checksummed account database and salted verifier survive an ATA reboot')


def test():
    test_the_crypto_and_policy_core()
    test_the_gate_and_protection_are_wired()
    test_a_booted_guest_enforces_accounts()
    test_a_persistent_account_survives_a_reboot()


if __name__ == '__main__':
    test()
