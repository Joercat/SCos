"""Interaction timer, system-app isolation rules and security-hardening suite.

Covers the bugs found in the audit: the 100 Hz PIT vs a 10x-too-short idle
timeout, inputs that did not reset the timer, the 32-bit seccomp truncation,
the FILE_WRITE offset wrap, path traversal in the private mount namespace,
unsanitised app ids and unguarded paint-after-OOM.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

# struct app layout (host-compiled from kernel/include/scos.h): size 144
APP_ID, APP_TITLE, APP_DEFW, APP_DEF_H = 0, 8, 24, 28
APP_MIN_W, APP_MIN_H, APP_USES_DATA, APP_EXTERNAL = 32, 36, 40, 104
APP_SIZE = 144


def key_event(g, code, pressed=1):
    g.debug.write(g.scratch, struct.pack('<BxH5Bx', 0, code, pressed, 0, 0, 0, 0))
    return g.call('handle_key', g.scratch)


def shell(g, line, cap=512):
    d, s = g.debug, g.scratch
    d.write(s + 8192, line.encode() + b'\0')
    d.write(s + 16384, b'\0' * cap)
    g.call('security_shell', s + 8192, s + 16384, cap)
    return g.string(s + 16384, cap)


def test_the_fixes_are_wired():
    sec = (ROOT / 'kernel/desktop/security.c').read_text()
    assert 'TICKS_PER_MINUTE 6000u' in sec, 'idle scale regressed to 6 s/min'
    assert 'security_idle_would_lock' in sec and 'security_note_activity' in sec
    for f in ('kernel/desktop/kbd.c', 'kernel/desktop/mouse.c', 'kernel/desktop/wm.c'):
        assert 'security_note_activity()' in (ROOT / f).read_text(), f + ' does not stamp activity'
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    assert '!a->external' in wm, 'system-app detection is not keyed on the external flag'
    assert 'wm_launch_app_override' in wm and 'Launch isolated' in wm
    assert 'int once = iso_once;' in wm, \
        'a one-launch override can leak into a nested window'
    apps = (ROOT / 'kernel/desktop/apps.c').read_text()
    assert 'strstr(a->id,"..")' in apps, 'app ids are not validated at registration'
    um = (ROOT / 'kernel/desktop/usermode.c').read_text()
    assert 'user_seccomp_allowed(nr)' in um, 'seccomp number is narrowed again'
    assert 'b >= sizeof(sh->scratch)' in um, 'FILE_WRITE offset wrap returned'
    assert '!active_pid' in um, 'capability check with no process context returned'
    assert 'draw_extent(c)' in um and 'draw_coord(a)' in um, \
        'CPL3 draw arguments are not clamped (CPU-render DoS / signed overflow)'
    assert 'cap_ok(writing ? PROC_CAP_VFS_WRITE' in um, \
        'CPL3 file syscalls do not check capabilities'
    vfs = (ROOT / 'kernel/desktop/vfs.c').read_text()
    assert "sc_neq(p, \"home/users\", 10)" in vfs
    settings = (ROOT / 'kernel/desktop/app_settings.c').read_text()
    assert 'Security' in settings and 'security_api_policy_set' in settings
    assert 'sec_y' in settings, 'no security section in Settings'
    print('PASS: idle scale, activity stamping, system-app rule, id validation, '
          'CPL3 bounds and Settings/terminal controls are wired')


def test_idle_timer_follows_interactions():
    from qemu_support import Guest  # noqa: E402

    with Guest('hardening-idle') as g:
        d, s = g.debug, g.scratch
        base = g.base
        assert g.call('security_api_policy_set', _str(g, s, 'idle'), 1) == 1
        assert g.call('security_api_idle_minutes') == 1
        # One minute = 6000 ticks at 100 Hz.  A 7-second idle must NOT lock;
        # the old 600-tick scale would have locked here.
        t = g.value('timer_ticks', 4)
        d.write(base + g.symbols['input_last_tick'], struct.pack('<I', (t - 700) & 0xffffffff))
        assert g.call('security_idle_would_lock') == 0, \
            'a 7-second idle locked a 1-minute policy (10x scale bug)'
        d.write(base + g.symbols['input_last_tick'], struct.pack('<I', (t - 7000) & 0xffffffff))
        assert g.call('security_idle_would_lock') == 1, 'a 70-second idle should lock'

        # ANY interaction resets it: keyboard, mouse press, mouse release with
        # no movement, and wheel.
        def idle_after(fn):
            t2 = g.value('timer_ticks', 4)
            d.write(base + g.symbols['input_last_tick'], struct.pack('<I', (t2 - 7000) & 0xffffffff))
            assert g.call('security_idle_would_lock') == 1
            fn()
            return g.call('security_idle_ticks')

        assert idle_after(lambda: key_event(g, 27)) < 100
        assert idle_after(lambda: g.call('mouse_inject', 1, 0, 0, 0)) < 100
        assert idle_after(lambda: g.call('mouse_inject', 0, 0, 0, 0)) < 100, \
            'a button release with no movement did not reset the timer'
        assert idle_after(lambda: g.call('mouse_inject', 0, 0, 0, 3)) < 100

        # Owner-facing commands and the side-effect-free predicate.
        assert 'Idle:' in shell(g, 'idle')
        assert 'Idle: 0' in shell(g, 'activity')
        assert g.call('security_api_policy_set', _str(g, s, 'idle'), 99999) == 0, \
            'an out-of-range policy value was accepted'
        g.call('security_api_policy_set', _str(g, s, 'idle'), 10)
        assert 'panic' not in g.serial().lower()
    print('PASS: 1 minute really is 1 minute, and every key/mouse interaction resets it')


def test_system_apps_and_isolation_rules():
    from qemu_support import Guest  # noqa: E402
    from test_cat import package  # noqa: E402

    with Guest('hardening-isolation') as g:
        d, s = g.debug, g.scratch
        # Every registered built-in is a system app: no prompt, unisolated.
        for i in range(g.call('app_count')):
            a = g.call('app_at', i)
            aid = g.ptr(a + APP_ID)
            if not aid:
                continue
            name = g.string(aid, 32)
            assert g.call('wm_app_is_system', aid) == 1, name
        d.write(s, b'notepad\0')
        before = g.call('wm_win_count')
        g.call('wm_launch_app_interactive', s, 0)
        assert g.value('menu', 4) == 0, 'system app showed the isolation prompt'
        w = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert g.call('wm_win_count') == before + 1 and w
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', w)) == 0

        # The ONLY way to isolate a system app is the explicit override.
        g.call('wm_launch_app_override', s, 0, 1)
        w2 = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert w2 != w
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', w2)) == 1
        # ... and a later normal launch is unisolated again (no sticky state).
        g.call('wm_launch_app_interactive', s, 0)
        w3 = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', w3)) == 0

        # Applies to ANY system app added later, not a hard-coded list.
        appmem = s + 4096
        d.write(appmem, b'\0' * APP_SIZE)
        d.write(s + 8192, b'future-app\0')
        d.write(s + 8256, b'Future App\0')
        d.write(appmem + APP_ID, struct.pack('<Q', s + 8192))
        d.write(appmem + APP_TITLE, struct.pack('<Q', s + 8256))
        d.write(appmem + APP_DEFW, struct.pack('<i', 400))
        d.write(appmem + APP_DEF_H, struct.pack('<i', 300))
        d.write(appmem + APP_MIN_W, struct.pack('<i', 320))
        d.write(appmem + APP_MIN_H, struct.pack('<i', 200))
        assert g.call('app_register', appmem) == 1
        assert g.call('wm_app_is_system', s + 8192) == 1
        g.call('wm_launch_app_interactive', s + 8192, 0)
        assert g.value('menu', 4) == 0, 'a newly registered system app prompted'
        w4 = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', w4)) == 0
        # Malicious ids cannot be registered at all.
        d.write(s + 8192, b'../evil\0')
        assert g.call('app_register', appmem) == 0

        # Installed apps with a stored choice never ask again and launch per it.
        g.save('home/apps/chosen.cat', package('return {}', id='chosen'))
        d.write(s, b'home/apps/chosen.cat\0')
        assert g.call('lua_app_install', s)
        d.write(s, b'chosen\0')
        assert g.call('security_app_isolation_set', s, 1) == 1   # always
        g.call('wm_launch_app_interactive', s, 0)
        assert g.value('menu', 4) == 0, 'a stored choice asked again'
        wc = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', wc)) == 1
        assert g.call('security_app_isolation_set', s, 2) == 1   # never
        g.call('wm_launch_app_interactive', s, 0)
        wn = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert g.call('proc_pid_isolated', g.call('proc_pid_for_window', wn)) == 0

        # Explicit context-menu override in BOTH directions.
        lab = s + 12288
        n = g.call('wm_desk_actions', 0, lab, 8)
        names = [g.string(g.ptr(lab + 8 * i), 40) for i in range(n)]
        assert 'Launch isolated' in names and 'Launch unisolated' in names, names
        act = g.call('wm_desk_action_index', 0, _str(g, s + 14000, 'Launch isolated'))
        assert act >= 0
        assert g.call('wm_desk_invoke', 0, act) == 0
        assert 'panic' not in g.serial().lower()
        # `isolate <id> ...` alias respects admin and stores the choice.
        assert 'set' in shell(g, 'isolate chosen never').lower()
        assert 'isolate' in shell(g, 'isolate').lower()
    print('PASS: system apps never prompt and default unisolated (including future '
          'ones); installed apps honour stored choices; explicit overrides work')


def test_cpl3_hostile_arguments_are_contained():
    from qemu_support import Guest  # noqa: E402

    with Guest('hardening-cpl3') as g:
        d, s = g.debug, g.scratch
        d.write(s, b'notepad\0')
        g.call('wm_launch_app_override', s, 0, 1)
        w = g.call('wm_win_at', g.call('wm_win_count') - 1)
        pid = g.call('proc_pid_for_window', w)
        assert pid and g.call('proc_domain', pid)

        # Full-width syscall number must not truncate to SC_NR_EXIT.
        assert g.call('user_seccomp_allowed', 1) == 1
        assert g.call('user_seccomp_allowed', 999) == 0
        assert g.call('user_seccomp_allowed', 0x100000001) == 0, \
            'a 64-bit syscall number truncated past the seccomp allowlist'

        # Run hostile syscalls from real CPL3 code and require a clean denial.
        for probe, label, expect in (
            (10, 'FILE_WRITE off=2^32-1 wrap', 0xffffffff),
            (11, '0x100000001 -> SC_NR_EXIT', 0xffffffff),
            (12, 'FILE_LIST huge offset', None),
            (13, 'mount namespace traversal', 0xffffffff),
        ):
            before = g.call('user_fault_count')
            d.write(s + 4096, b'\0' * 8)
            r = g.call('user_sandbox_probe', pid, probe, s + 4096)
            res = int.from_bytes(d.read(s + 4096, 4), 'little')
            assert r == 1, f'{label}: CPL3 episode did not finish cleanly'
            assert g.call('user_fault_count') == before, f'{label}: faulted the CPU'
            if expect is not None:
                assert res == expect, f'{label}: result {res:#x} != {expect:#x}'
            else:
                assert res in (0, 0xff, 0xffffffff), f'{label}: odd result {res:#x}'

        # Per-account private files: the parent must not enumerate accounts,
        # traversal must be refused, and another user's tree must be denied.
        assert g.call('vfs_access', _str(g, s, 'home/users'), 0) == 0
        assert g.call('vfs_access', _str(g, s, 'home/users/root/../other/x'), 0) == 0
        assert g.call('vfs_access', _str(g, s, 'home/users/other/secret'), 0) == 0
        assert 'panic' not in g.serial().lower()
    print('PASS: seccomp truncation, offset wrap, list bounds and namespace '
          'traversal are refused from real CPL3 code without a fault')


def _str(g, at, text):
    g.debug.write(at, text.encode() + b'\0')
    return at


def test():
    test_the_fixes_are_wired()
    test_idle_timer_follows_interactions()
    test_system_apps_and_isolation_rules()
    test_cpl3_hostile_arguments_are_contained()


if __name__ == '__main__':
    test()
