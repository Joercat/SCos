"""SCos process isolation: real page-table domains, private app memory, a
kernel-held capability set and per-account private files.

The old implementation was a boolean on `struct window` that only a handful of
Lua API functions looked at.  These tests exercise the replacement: an
isolated app's private pages must be *absent* from another process's page
tables (proved with an actual CR3 switch and a read), and the VFS/theme/IPC
checks must apply to native C apps as well as CAT packages.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

PROC_CAP_VFS_READ = 0x01
PROC_CAP_VFS_WRITE = 0x02
PROC_CAP_THEME = 0x04
PROC_CAP_IPC = 0x08
PROC_CAP_APPCTL = 0x10
MARKER = 0x53434F5350524F43


def test_the_isolation_engine_is_wired():
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    assert '#define APP_T0(w)' in wm and 'proc_enter((w))' in wm and 'proc_leave(proc_tok)' in wm, \
        'app callbacks no longer run inside a process context'
    assert 'proc_create(w)' in wm and 'proc_destroy(w)' in wm, \
        'windows are not registered as processes'
    mem = (ROOT / 'kernel/src/memory.c').read_text()
    assert 'memory_domain_switch' in mem and 'identity_hide' in mem, \
        'page-table domains no longer remove the identity-map alias'
    assert 'mov %0,%%cr3' in mem, 'no CR3 switch exists'
    proc = (ROOT / 'kernel/desktop/process.c').read_text()
    assert 'proc_pid_path_allowed' in proc and 'proc_pid_cap_allowed' in proc
    assert 'proc_private_alloc' in proc and 'proc_pid_probe' in proc
    vfs = (ROOT / 'kernel/desktop/vfs.c').read_text()
    assert 'proc_path_allowed' in vfs and 'vfs_user_private' in vfs, \
        'VFS no longer enforces the process namespace or per-account files'
    assert 'int vfs_access' in vfs
    lua = (ROOT / 'kernel/desktop/lua_apps.c').read_text()
    assert 'proc_private_alloc' in lua and 'cat_isolation_guard' in lua
    assert 'd->usermode' in lua and 'user_run_event' in lua, \
        'isolated CAT packages are not dispatched through the CPL3 runtime'
    assert 'sc_syscall_entry' in (ROOT / 'kernel/x86_64/usermode.S').read_text()
    assert 'usermode_cpu_init' in (ROOT / 'kernel/src/interrupt.c').read_text()
    assert 'SC_NR_EXIT' in (ROOT / 'kernel/user/user_syscalls.h').read_text()
    api = (ROOT / 'kernel/desktop/lua_api.inc').read_text()
    assert 'PROC_CAP_THEME' in api and 'PROC_CAP_IPC' in api and 'PROC_CAP_APPCTL' in api
    assert 'proc_cap_allowed(PROC_CAP_THEME)' in (ROOT / 'kernel/desktop/theme.c').read_text()
    sec = (ROOT / 'kernel/desktop/security.c').read_text()
    assert 'user_dirs_init()' in sec and 'proc_isolation_count()' in sec
    header = (ROOT / 'kernel/include/process.h').read_text()
    assert 'PROC_CAP_VFS_READ' in header and 'PROC_CAP_ALL' in header
    ata = (ROOT / 'kernel/drivers/ata.c').read_text()
    assert 'rollback_check(vault_generation())' in ata and 'rollback_bump' in ata, \
        'saved-image rollback detection is not wired'
    print('PASS: domains, CR3 switch, capability gates, VFS namespace, '
          'per-user files and anti-rollback are wired')


def test_a_booted_guest_enforces_isolation():
    from qemu_support import Guest  # noqa: E402
    from test_cat import package  # noqa: E402

    with Guest('isolation') as g:
        d, s = g.debug, g.scratch

        def put(off, text):
            d.write(s + off, text.encode() + b'\0')
            return s + off

        # Two isolated native C apps.  They are compiled-in ring-0 code, so
        # isolation has to be enforced by the kernel, not by their cooperation.
        a1 = put(0, 'notepad')
        g.call('wm_launch_app_override', a1, 0, 1)
        w1 = g.call('wm_win_at', g.call('wm_win_count') - 1)
        a2 = put(256, 'about')
        g.call('wm_launch_app_override', a2, 0, 1)
        w2 = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert w1 and w2
        p1, p2 = g.call('proc_pid_for_window', w1), g.call('proc_pid_for_window', w2)
        assert p1 and p2 and p1 != p2
        assert g.call('proc_count') == 2 and g.call('proc_isolation_count') == 2
        d1, d2 = g.call('proc_domain', p1), g.call('proc_domain', p2)
        assert d1 and d2 and d1 != d2, 'both apps share one page-table domain'
        # Every CPL3 process sees USER_BASE at ITS OWN physical frames.  The
        # same virtual address therefore contains a different marker in each
        # process, and no VA in p2's PML4 resolves to p1's frames.
        user0 = 0x600000000000
        out = s + 4096
        assert g.call('proc_pid_probe', p1, user0, out) == 1
        first = int.from_bytes(d.read(out, 8), 'little')
        assert first == MARKER + p1
        assert g.call('proc_pid_probe', p2, user0, out) == 1
        second = int.from_bytes(d.read(out, 8), 'little')
        assert second == MARKER + p2 and second != first, \
            'both processes resolve USER_BASE to the same physical memory'
        assert g.call('proc_kernel_root') not in (0, user0)
        # The CPL3 image, stack and shared page are present; an address past
        # the mapped slot is not.
        assert g.call('user_pid_mapped', p1, 0x600000040000) == 1
        assert g.call('user_pid_mapped', p1, 0x600000700000) == 0
        assert g.call('user_image_entry') == 0x600000040000
        assert g.call('usermode_enabled') == 1

        # Capabilities are kernel-held and cannot be forged by app code.
        assert g.call('proc_pid_cap_allowed', p1, PROC_CAP_VFS_READ) == 1
        assert g.call('proc_pid_cap_allowed', p1, PROC_CAP_VFS_WRITE) == 1
        assert g.call('proc_pid_cap_allowed', p1, PROC_CAP_THEME) == 0
        assert g.call('proc_pid_cap_allowed', p1, PROC_CAP_IPC) == 0
        assert g.call('proc_pid_cap_allowed', p1, PROC_CAP_APPCTL) == 0

        # The real VFS access path, evaluated AS the process (not a helper).
        assert g.call('proc_pid_access', p1, put(512, 'home/documents/welcome.txt'), 0) == 0
        assert g.call('proc_pid_access', p1, put(512, 'system/settings.json'), 1) == 0
        assert g.call('proc_pid_access', p1, put(512, 'home/appdata/root/about/secret'), 1) == 0
        assert g.call('proc_pid_access', p1, put(512, 'home/appdata/root/notepad/x'), 1) == 1
        assert g.call('proc_pid_access', p1, put(512, 'home/appdata/root/notepad'), 1) == 1
        assert g.call('proc_pid_access', 0, put(512, 'home/documents/welcome.txt'), 0) == 1
        # A real mount namespace: vfs_lookup honours "..", so traversal,
        # nested paths and "." segments must all be refused before any lookup.
        for escape in ('home/appdata/root/notepad/../about/x',
                       'home/appdata/root/notepad/./x',
                       'home/appdata/root/notepad/sub/x',
                       'home/appdata/root/notepad/../../documents/welcome.txt',
                       'home/apps/notepad.cat/../x'):
            assert g.call('proc_pid_access', p1, put(512, escape), 1) == 0, \
                'namespace traversal accepted: ' + escape

        # Per-account private files: root can use home/users/root/**, but not
        # another account's tree, even with no app running.
        assert g.call('vfs_lookup', put(512, 'home/users/root')) != 0
        assert g.call('vfs_access', put(512, 'home/users/root/private.txt'), 1) == 1
        assert g.call('vfs_access', put(512, 'home/users/someoneelse/private.txt'), 1) == 0

        # An app the owner chose NOT to isolate keeps the full API.
        a3 = put(768, 'calendar')
        w3 = g.call('wm_open_app', a3, 0)   # system app: unisolated by default
        p3 = g.call('proc_pid_for_window', w3)
        assert p3 and g.call('proc_pid_isolated', p3) == 0
        assert g.call('proc_domain', p3) == 0, 'a non-isolated app was forced into a domain'
        assert g.call('proc_pid_access', p3, put(512, 'home/documents/welcome.txt'), 0) == 1
        assert g.call('proc_pid_cap_allowed', p3, PROC_CAP_THEME) == 1

        # A third-party CAT package gets its 2 MiB Lua heap INSIDE its own
        # domain and can still read/write its private appdata namespace.
        src = "local ok=scos.write('iso.txt','private data');assert(ok);assert(scos.read('iso.txt')=='private data');return {}"
        g.save('home/apps/iso-app.cat', package(src, id='iso-app'))
        assert g.call('lua_app_install', put(1024, 'home/apps/iso-app.cat'))
        a4 = put(1280, 'iso-app')
        assert g.call('security_app_isolation_set', a4, 1) == 1
        w4 = g.call('wm_open_app', a4, 0)
        assert w4
        g.run(0.5)
        p4 = g.call('proc_pid_for_window', w4)
        assert g.call('proc_pid_isolated', p4) == 1
        assert g.call('user_pid_mapped', p4, 0x600000040000) == 1
        assert g.state(w4) == (0, ''), g.state(w4)
        assert g.read('home/appdata/root/iso-app/iso.txt') == b'private data'
        # Its Lua VM, heap and bytecode executed at CPL3 through syscalls.
        assert g.call('user_syscall_count') > 0
        assert g.call('user_fault_count') == 0

        # The shell reports the same state to the owner.
        assert 'isolated with private page-table domains' in shell(g, 'security')
        assert 'iso-app' in shell(g, 'isolation')

        free_before = g.call('memory_free_pages')
        for w in (w1, w2, w3, w4):
            g.call('wm_close_window', w)
        assert g.call('proc_count') == 0, 'processes leaked after close'
        assert g.call('memory_free_pages') > free_before, \
            'private domain pages were not released'
        assert 'panic' not in g.serial().lower()
    print('PASS: isolated apps have separate PML4s, private pages, capabilities, '
          'VFS namespaces and per-account files; unisolated apps are unchanged')


def shell(g, line):
    d, s = g.debug, g.scratch
    d.write(s + 8192, line.encode() + b'\0')
    d.write(s + 16384, b'\0' * 2048)
    g.call('security_shell', s + 8192, s + 16384, 2048)
    return g.string(s + 16384, 2048)


def test_rollback_of_a_saved_image_is_detected():
    """Restoring an OLDER authenticated vault image must be refused.  The
    generation lives in the AEAD header; the high-water mark lives in a sector
    outside the 256-sector image, so copying the image back cannot reset it."""
    from qemu_support import Guest  # noqa: E402

    with Guest('rollback', ide=True) as g:
        d, s = g.debug, g.scratch
        img = g.ptr(g.base + g.symbols['img_buf'])
        assert g.call('fs_image_save') == 1
        old = d.read(img, 131072)
        assert old[:8] == b'SCOSVAUL'
        first = g.call('rollback_high_water')
        assert g.call('rollback_valid') == 1 and first > 0

        assert g.call('fs_image_save') == 1
        high = g.call('rollback_high_water')
        assert high > first
        assert g.call('rollback_check', high) == 1
        assert g.call('rollback_check', first) == 0, \
            'an older generation is still accepted'
        assert g.call('ata_read_sectors', 64, 1, s + 4096) == 1
        assert d.read(s + 4096, 10) == b'SCOSEPOCH1', 'anchor is not on disk'

        # Replay just the image (the attack this defends against), then reboot.
        d.write(img, old)
        assert g.call('ata_write_sectors', 129024, 256, img) == 1
        g.reboot()
        assert 'rollback' in g.serial().lower(), \
            'an older vault image was accepted after a replay: ' + g.serial()[-1500:]
        assert g.call('rollback_high_water') >= high
        assert 'panic' not in g.serial().lower()
    print('PASS: an older but still authenticated vault image is rejected '
          'using the off-image generation anchor')


def test():
    test_the_isolation_engine_is_wired()
    test_a_booted_guest_enforces_isolation()
    test_rollback_of_a_saved_image_is_detected()


if __name__ == '__main__':
    test()
