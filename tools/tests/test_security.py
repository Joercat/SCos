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
    objects = []
    for name in ('sec_core', 'sec_aes'):
        obj = out.parent / (name + '_host.o')
        run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-c',
                                     'kernel/desktop/%s.c' % name, '-o', str(obj)])
        objects.append(str(obj))
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include',
                                 'tools/tests/security_host.c'] + objects + ['-o', str(out)])
    result = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.startswith(('PASS', 'FAIL', 'TOTAL'))]
    print('\n'.join(lines))
    assert result.returncode == 0, result.stdout[-3000:]
    assert not [l for l in lines if l.startswith('FAIL')], result.stdout
    assert len([l for l in lines if l.startswith('PASS')]) >= 86, \
        'the security core suite shrank: %d checks' % len(lines)
    print('PASS: SHA-256, HMAC, PBKDF2, AES-256/CTR, AEAD, VEK wrap, '
          'policy, codec and checksum vectors')


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
    assert 'proc_path_allowed' in vfs and 'vfs_user_private' in vfs, \
        'process namespaces and per-account private files are not enforced at the VFS boundary'
    term = (ROOT / 'kernel/desktop/app_terminal.c').read_text()
    assert 'security_current_user()' in term, 'the terminal prompt claims a fake user again'
    assert 'security_command_allowed(' in term, 'terminal commands bypass authorization'
    tty = (ROOT / 'kernel/desktop/tty.c').read_text()
    assert 'security_reauthenticate(' in tty, 'the maintenance console has no re-auth gate'
    assert 'Rescue console' in tty and 'if (return_to_wm && !security_reauthenticate' not in tty, \
        'rescue mode still inherits an authenticated session'
    header = (ROOT / 'kernel/include/security.h').read_text()
    for needle in ('sc_pbkdf2', 'sc_password_eval', 'sc_db_verify', 'SC_FLAG_MUSTCHANGE'):
        assert needle in header, '%s disappeared from the account API' % needle
    core = (ROOT / 'kernel/desktop/sec_core.c').read_text()
    assert 'trailing separator' in core, 'the empty-history reboot regression was removed'
    assert '"root", "nobody"' not in core, 'the default username root is reserved again'
    account = (ROOT / 'kernel/desktop/security.c').read_text()
    assert 'vault_unlock_password' in account and 'fs_image_unlock_apply' in account, \
        'sign-in no longer unlocks the encrypted disk'
    assert 'vault_set_db(db_buf' in account and 'vault_set_audit' in account, \
        'account state is no longer authenticated as vault associated data'
    ata = (ROOT / 'kernel/drivers/ata.c').read_text()
    assert 'vault_seal(img_buf' in ata and 'vault_open(img_buf' in ata, \
        'the ATA image is no longer encrypted'
    assert 'SCOSFS2' in ata and 'legacy plaintext' in ata, 'legacy migration was removed'
    # User choices, plain console and per-app isolation choice plumbing.
    choices = (ROOT / 'kernel/desktop/sec_choices.c').read_text()
    for needle in ('login.required', 'lock.enabled', 'password.policy', 'guest.restrict',
                   'vault.encryption', 'isolation.prompt', 'confirm.destructive'):
        assert needle in choices, '%s is no longer a user choice' % needle
    assert 'security_choice_set' in choices and 'security_choices_save' in choices, \
        'choices are not persisted through an app-facing API'
    loginui = (ROOT / 'kernel/desktop/login.c').read_text()
    assert 'con_put' in loginui and 's_vgrad' not in loginui and 's_frame_rect' not in loginui, \
        'the login screen is a graphical panel again instead of a plain TTY console'
    assert 'security_set_lock_screen' in loginui, 'no documented API for a user-made lock-screen app'
    # Real process isolation (not just a flag) and off-image rollback detection.
    assert 'proc_enter((w))' in wm and 'proc_create(w)' in wm, \
        'app callbacks are not wrapped in a process/page-table domain'
    assert 'memory_domain_switch' in (ROOT / 'kernel/src/memory.c').read_text()
    assert 'proc_private_alloc' in (ROOT / 'kernel/desktop/lua_apps.c').read_text()
    assert 'rollback_check(vault_generation())' in ata and 'EPOCH_LBA 64' in ata, \
        'an older authenticated vault image can be replayed without detection'
    # A real CPL3 boundary, not a ring-0 capability flag.
    assert 'sysretq' in (ROOT / 'kernel/x86_64/usermode.S').read_text()
    assert 'EFER.SCE' in (ROOT / 'kernel/src/interrupt.c').read_text()
    assert 'd->usermode' in (ROOT / 'kernel/desktop/lua_apps.c').read_text(), \
        'isolated CAT packages are not dispatched at CPL3'
    assert 'user_seccomp_allowed' in (ROOT / 'kernel/desktop/usermode.c').read_text()
    assert 'custom_lock_screen' in loginui
    assert 'security_choice("lock.enabled")' in loginui
    assert 'wm_launch_app_interactive' in wm and 'iso_items' in wm, \
        'the WM first-launch isolation prompt was removed'
    assert 'w->isolated' in wm and 'cat_isolation_guard' in (ROOT / 'kernel/desktop/lua_apps.c').read_text(), \
        'the per-app isolation flag is not honoured anywhere'
    assert 'int  isolated;' in (ROOT / 'kernel/include/scos.h').read_text()
    assert 'confirm.destructive' in (ROOT / 'kernel/desktop/confirm.c').read_text()
    # Default account, top-level equal permissions and a Linux-like TTY login.
    assert 'SC_DEFAULT_USER "root"' in header and 'SC_DEFAULT_PASS "scos"' in header
    assert 'SC_DEFAULT_ROLE SC_ROLE_ADMIN' in header
    account_src = (ROOT / 'kernel/desktop/security.c').read_text()
    assert 'create_default_account' in account_src and 'a->role = SC_ROLE_ADMIN' in account_src, \
        'the default account is not at the highest permission level'
    assert 'int role = SC_ROLE_ADMIN;' in account_src, \
        'new accounts no longer default to top permission'
    assert '!security_choice("roles.enabled")' in account_src, \
        'accounts do not have equal top permissions by default'
    assert 'scos login' in loginui and 'SCos tty1' in loginui, \
        'the login prompt is not a plain Linux-style tty'
    assert 'roles.enabled' in choices and 'login.hint' in choices
    # No Secure Boot / signed boot anywhere in the tree.
    for path in ('boot/uefi/main.c', 'tools/makedisk.py', 'Makefile',
                 'kernel/include/security.h', 'kernel/desktop/security.c'):
        assert 'Secure Boot' not in (ROOT / path).read_text() and \
               'BOOT.SIG' not in (ROOT / path).read_text(), \
            'boot signing / Secure Boot came back in %s' % path
    print('PASS: boot gate, WM hook, VFS guard, plain TTY console, user choices, '
          'isolation prompt and codec regression are wired')


def test_a_booted_guest_enforces_accounts():
    from qemu_support import Guest  # noqa: E402

    pw = b'scos'
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
        assert g.string(g.call('security_current_user'), 64) == 'root'
        assert g.call('security_api_default_credentials') == 1, \
            'the shipped root/scos default credentials were not provisioned'
        assert g.call('security_is_admin') == 1       # top permission
        d.write(s, b'roles.enabled\0')
        assert g.call('security_choice', s) == 0, 'role privileges are forced on by default'
        assert 'administrator (all accounts equal)' in shell('security')[1]
        assert 'administrator (all accounts equal)' in shell('id')[1]

        # A wrong password and an unknown user both fail with the same message.
        assert auth('root', b'wrong') == 0
        assert auth('ghost', b'wrong') == 0
        assert auth('root', pw) == 1
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
        assert g.call('security_api_default_credentials') == 0, \
            'the default-credential hint is still active after passwd'
        assert auth('root', pw) == 0 and auth('root', p1) == 1
        d.write(s + 8192, weak + b'\0')
        assert g.call('security_api_change_password', 0, 0, s + 8192, 0, s + 16384, 192) == 0

        # Guest accounts are read-only in the shell and at the VFS boundary
        # when (and only when) the owner opts into role differences.
        d.write(s + 8192, b'viewer\0')
        d.write(s + 16384, b'Km4!dV9#pLq2Xz7%' + b'\0')
        assert g.call('security_api_create_user', s + 8192, 2, s + 16384) == 1
        d.write(s, b'roles.enabled\0')
        assert g.call('security_choice_set', s, 1) == 1
        assert auth('viewer', b'Km4!dV9#pLq2Xz7%') == 1
        assert g.call('security_api_force_change') == 1          # admin-created accounts must change
        assert allowed('ls')[0] == 1
        assert allowed('touch x')[0] == 0 and allowed('save')[0] == 0
        d.write(s + 8192, b'guest.txt\0')
        d.write(s + 16384, b'data\0')
        assert g.call('vfs_write', s + 8192, s + 16384, 4) == 0
        assert auth('root', p1) == 1
        assert g.call('security_is_admin') == 1, \
            'the base account is not top power when roles are enforced'
        d.write(s, b'roles.enabled\0')
        assert g.call('security_choice_set', s, 0) == 1
        assert allowed('save')[0] == 1, 'equal permissions do not apply to all commands'

        assert 'panic' not in g.serial().lower(), g.serial()[-2000:]
    print('PASS: booted guest authenticates, protects its database, changes passwords, '
          'rejects reuse and enforces read-only guests')


def test_a_persistent_account_survives_a_reboot():
    from qemu_support import Guest  # noqa: E402

    pw = b'scos'
    with Guest('security-ata', ide=True) as g:
        assert g.call('security_api_persistent') == 1
        assert g.call('fs_image_save') == 1
        g.run(0.8)
        g.reboot()
        assert g.call('security_api_user_count') == 1
        g.debug.write(g.scratch, b'root\0')
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


def test_a_booted_vault_is_encrypted_and_tamper_evident():
    """The saved image must hold no plaintext, and the guest's own AEAD must
    reject edits to its header, tag and ciphertext - not just the host unit
    tests.  Signing out must make a plaintext save impossible."""
    from qemu_support import Guest  # noqa: E402

    canary = b'VAULT-PLAINTEXT-CANARY-9283'
    with Guest('security-vault', ide=True) as g:
        d, s = g.debug, g.scratch
        assert g.call('vault_unlocked') == 1, 'sign-in did not unlock the volume key'
        g.save('home/documents/secret.txt', canary)
        assert g.call('fs_image_save') == 1
        image = d.read(g.ptr(g.base + g.symbols['img_buf']), 131072)
        assert image[:8] == b'SCOSVAUL', 'the disk image is not an encrypted vault'
        assert b'SCOSFS2' not in image, 'a plaintext FS2 header is on the disk'
        assert canary not in image, 'a saved document is readable in cleartext on the disk'

        work = s + 131072

        def opens_after(edit):
            d.write(work, image)
            if edit is not None:
                d.write(work + edit, bytes([image[edit] ^ 1]))
            return g.call('vault_open', work, 131072)

        assert opens_after(None) == 1, 'the genuine vault does not authenticate'
        assert opens_after(3000) == 0, 'editing the cleartext account header was not detected'
        assert opens_after(32740) == 0, 'editing the AEAD tag was not detected'
        assert opens_after(60000) == 0, 'editing the ciphertext was not detected'

        g.call('vault_lock')
        assert g.call('fs_image_save') == 0, 'a locked vault still wrote an image'
        d.write(s, b'root\0')
        d.write(s + 64, b'scos\0')
        d.write(s + 128, b'\0' * 192)
        assert g.call('security_api_authenticate', s, s + 64, s + 128, 192) == 1
        assert g.call('vault_unlocked') == 1, 're-authentication did not re-wrap the volume key'
        assert g.call('fs_image_save') == 1
    print('PASS: the saved image is encrypted, metadata/tag/data are authenticated, '
          'and a logged-out vault refuses to write')


def test_security_are_user_choices_and_isolation_is_per_app():
    """Choices default on, can be turned off, survive a reboot, and actually
    change behaviour.  Isolation is a recorded per-app choice, never forced."""
    from qemu_support import Guest  # noqa: E402

    with Guest('security-choices', ide=True) as g:
        d, s = g.debug, g.scratch

        def choice(key):
            d.write(s, key.encode() + b'\0')
            return g.call('security_choice', s)

        def choice_set(key, value):
            d.write(s, key.encode() + b'\0')
            return g.call('security_choice_set', s, value)

        def shell(line):
            d.write(s + 8192, line.encode() + b'\0')
            d.write(s + 16384, b'\0' * 2048)
            g.call('security_shell', s + 8192, s + 16384, 2048)
            return g.string(s + 16384, 2048)

        # The QEMU harness turns isolation.prompt off so unrelated UI suites
        # keep their original launch paths.  Restore it here to test the real
        # default and the WM prompt (this is also a user choice, not a bypass).
        assert choice_set('isolation.prompt', 1) == 1
        for key in ('login.required', 'lock.enabled', 'lock.idle', 'password.policy',
                    'password.age', 'guest.restrict', 'audit.enabled',
                    'vault.encryption', 'isolation.prompt', 'confirm.destructive'):
            assert choice(key) == 1, '%s is not enabled by default' % key
        assert choice('isolation.default') == 0, 'isolation is forced by default'
        assert choice('roles.enabled') == 0, 'all accounts are not equal by default'
        assert choice('login.hint') == 1
        assert 'ALL are the owner' in shell('choices'), 'choices command does not list switches'

        # Turning a protection off really changes behaviour (not just a flag).
        assert choice_set('lock.enabled', 0) == 1
        g.call('security_api_lock_session')
        assert g.call('security_api_locked') == 0, 'lock.enabled=off still locked the session'
        assert choice_set('lock.enabled', 1) == 1
        g.call('security_api_lock_session')
        assert g.call('security_api_locked') == 1, 'lock.enabled=on no longer locks'
        g.call('security_api_unlock_session')
        # Choices live in the AEAD-authenticated vault header, so they survive a reboot.
        assert choice_set('lock.idle', 0) == 1
        assert g.call('fs_image_save') == 1
        g.run(0.5)
        g.reboot()
        d, s = g.debug, g.scratch
        assert choice('lock.idle') == 0, 'a user choice did not survive reboot'
        assert choice('login.required') == 1, 'other choices were lost on reboot'
        assert choice_set('lock.idle', 1) == 1
        assert choice_set('isolation.prompt', 1) == 1   # harness opts out on connect

        # Per-app isolation is a choice, and the WM exposes it to an app.
        d.write(s, b'counter\0')
        assert g.call('security_app_isolation', s) == 0, 'a new app is not asked'
        assert g.call('security_app_isolation_pending', s) == 1
        assert g.call('security_app_isolation_set', s, 1) == 1
        assert g.call('security_app_isolation', s) == 1
        assert g.call('security_app_isolation_pending', s) == 0
        assert g.call('security_app_last_isolated', s) == 0, 'a stored choice forces isolation'
        assert g.call('security_app_launch_isolated', s, 1) == 1
        assert g.call('security_app_last_isolated', s) == 1
        assert 'always isolated' in shell('choices'), 'per-app choice is not listed'

        # A real user-driven WM launch shows the four-way prompt on first open.
        d.write(s, b'files\0')
        assert g.call('security_app_isolation_pending', s) == 1
        g.call('wm_launch_app_interactive', s, 0)
        assert g.value('menu', 4) == 1, 'no first-launch isolation prompt in the WM'
        g.call('iso_answer', 1, 0)          # "Never launch isolated"
        assert g.call('security_app_isolation', s) == 2, 'the menu answer was not stored'
        assert g.call('wm_win_count') >= 1, 'the app did not launch after the choice'
        d.write(s, b'sketch\0')
        assert choice_set('isolation.prompt', 0) == 1
        assert g.call('security_app_isolation_pending', s) == 0, \
            'the prompt choice does not disable the prompt'

        # Encryption is a choice too: turning it off writes a plain FS2 image.
        assert choice_set('vault.encryption', 0) == 1
        assert g.call('fs_image_save') == 1
        img = d.read(g.ptr(g.base + g.symbols['img_buf']), 131072)
        assert img[:7] == b'SCOSFS2', ('vault.encryption=off did not produce a plain image: '
                                      'choice=%d magic=%r' % (choice('vault.encryption'), img[:16]))
        assert choice_set('vault.encryption', 1) == 1
    print('PASS: security choices default on, persist across reboot, change behaviour, '
          'and per-app isolation is a user decision')


def test():
    test_the_crypto_and_policy_core()
    test_the_gate_and_protection_are_wired()
    test_a_booted_guest_enforces_accounts()
    test_a_persistent_account_survives_a_reboot()
    test_a_booted_vault_is_encrypted_and_tamper_evident()
    test_security_are_user_choices_and_isolation_is_per_app()


if __name__ == '__main__':
    test()
