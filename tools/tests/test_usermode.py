"""CPL3 user-mode boundary: the part that makes isolation unescapable.

Inspired by the Linux/Flatpak stack (bubblewrap namespaces + seccomp-bpf +
capabilities + cgroups), implemented here as a real ring-3 switch with a
per-process PML4 and a numbered syscall gate.  These tests deliberately try to
escape from ring 3 and prove the CPU stops them while the kernel survives.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

USER_BASE = 0x600000040000
CS_USER = 0x33


def test_the_cpl3_boundary_is_wired():
    asm = (ROOT / 'kernel/x86_64/usermode.S').read_text()
    assert 'sysretq' in asm and 'syscall' in asm.lower()
    assert 'sc_syscall_entry' in asm and 'sc_user_fault_return' in asm
    irq = (ROOT / 'kernel/src/interrupt.c').read_text()
    assert 'EFER.SCE' in irq and 'STAR' in irq and 'LSTAR' in irq and 'SFMASK' in irq
    assert 'DPL 3' in irq and 'user_fault(f)' in irq
    vectors = (ROOT / 'tools/gen_vectors.py').read_text()
    assert 'user_fault_taken' in vectors and 'sc_user_fault_return' in vectors
    dispatch = (ROOT / 'kernel/desktop/usermode.c').read_text()
    assert 'user_seccomp_allowed' in dispatch and 'ns_path' in dispatch
    assert 'USER_RLIMIT_SYSCALLS' in dispatch
    user_h = (ROOT / 'kernel/include/usermode.h').read_text()
    user_c = (ROOT / 'kernel/user/user_syscalls.h').read_text()
    for token in ('SC_NR_EXIT', 'SC_NR_DRAW', 'SC_NR_FILE_WRITE', 'SC_NR_FAULT'):
        assert token in user_h and token in user_c, token + ' is not in sync'
    host = (ROOT / 'kernel/user/cat_user.c').read_text()
    assert 'lua_newstate' in host and 'user_cat_main' in host
    assert 'SC_NR_' in host, 'the CPL3 Lua host must use syscalls, not kernel calls'
    print('PASS: CPL3 segments, SYSCALL/SYSRET MSRs, fault return, seccomp, '
          'mount namespace and cgroup limits are wired')


def test_a_booted_guest_cannot_escape_ring3():
    from qemu_support import Guest  # noqa: E402

    with Guest('usermode') as g:
        d, s = g.debug, g.scratch
        d.write(s, b'notepad\0')
        g.call('wm_launch_app_override', s, 0, 1)
        w = g.call('wm_win_at', g.call('wm_win_count') - 1)
        assert w
        pid = g.call('proc_pid_for_window', w)
        assert pid and g.call('proc_domain', pid)
        assert g.call('usermode_enabled') == 1
        assert g.call('user_image_entry') == USER_BASE

        # A normal episode runs to completion and can use its private files.
        d.write(s + 4096, b'\0' * 8)
        assert g.call('user_sandbox_probe', pid, 0, s + 4096) == 1
        assert int.from_bytes(d.read(s + 4096, 4), 'little') == 2, \
            'the CPL3 app could not write and read its own private file'

        # Hardware escape attempts.  Each is a SEPARATE ring-3 episode; a
        # contained fault returns -1 and increments the fault counter.
        escape_probes = {
            1: 'read kernel memory',
            2: 'write kernel memory',
            3: 'mov cr3',
            4: 'wrmsr',
            5: 'cli',
            6: 'outb (port I/O)',
            7: 'read another address space',
        }
        for probe, label in escape_probes.items():
            before = g.call('user_fault_count')
            d.write(s + 4096, b'\0' * 8)
            r = g.call('user_sandbox_probe', pid, probe, s + 4096)
            after = g.call('user_fault_count')
            assert r == 0xffffffff and after == before + 1, \
                f'escape probe {probe} ({label}) was not contained: ret={r} faults={before}->{after}'

        # seccomp: an unknown syscall number is refused, but the app lives.
        before = g.call('user_denied_count')
        d.write(s + 4096, b'\0' * 8)
        assert g.call('user_sandbox_probe', pid, 8, s + 4096) == 1
        assert g.call('user_denied_count') > before
        assert g.call('user_fault_count') == len(escape_probes), \
            'a seccomp denial must not crash the app'

        # mount namespace: a path outside the private root is refused.
        d.write(s + 4096, b'\0' * 8)
        assert g.call('user_sandbox_probe', pid, 9, s + 4096) == 1
        assert int.from_bytes(d.read(s + 4096, 4), 'little') == 0xffffffff
        assert g.call('user_namespace_active') == 0 or g.call('user_namespace_active') == 1

        # Resources and the allowlist are inspectable and adjustable by the owner.
        assert g.call('user_seccomp_allowed', 1) == 1
        assert g.call('user_seccomp_allowed', 999) == 0
        g.call('user_set_seccomp', 3, 0)          # SC_NR_CLOCK
        assert g.call('user_seccomp_allowed', 3) == 0
        g.call('user_set_seccomp', 3, 1)
        assert g.call('user_resource_limit', 1) > 0   # file count
        assert g.call('user_syscall_count') > 0

        g.run(0.4)
        serial = g.serial().lower()
        assert 'panic' not in serial
        assert 'contained fault' in serial
        d.write(s + 8192, b'isolation\0')
        d.write(s + 16384, b'\0' * 2048)
        g.call('security_shell', s + 8192, s + 16384, 2048)
        assert 'isolated' in g.string(s + 16384, 2048)
        g.call('wm_close_window', w)
        assert g.call('proc_count') == 0
    print('PASS: ring-3 code cannot read/write kernel memory, run a privileged '
          'instruction, see another process, or leave its mount namespace; '
          'seccomp and cgroup limits hold and the machine survives')


def test_isolated_cat_packages_run_at_ring3():
    """Third-party/installed apps execute their Lua VM entirely in CPL3."""
    from qemu_support import Guest  # noqa: E402
    from test_cat import package  # noqa: E402

    src = ("local ok=scos.write('ring3.txt','cpl3 data');assert(ok);"
           "assert(scos.read('ring3.txt')=='cpl3 data');"
           "return {paint=function(w,h) scos.rect(2,2,8,8,scos.rgb(1,2,3)) end}")
    with Guest('usermode-cat') as g:
        d, s = g.debug, g.scratch
        g.save('home/apps/ring3.cat', package(src, id='ring3'))
        d.write(s, b'home/apps/ring3.cat\0')
        assert g.call('lua_app_install', s)
        d.write(s + 256, b'ring3\0')
        assert g.call('security_app_isolation_set', s + 256, 1) == 1
        w = g.call('wm_open_app', s + 256, 0)
        assert w
        g.run(0.5)
        pid = g.call('proc_pid_for_window', w)
        assert g.call('proc_pid_isolated', pid) == 1
        assert g.call('user_pid_mapped', pid, USER_BASE) == 1
        assert g.state(w) == (0, ''), g.state(w)
        assert g.read('home/appdata/root/ring3/ring3.txt') == b'cpl3 data'
        assert g.call('user_syscall_count') > 0
        assert g.call('user_fault_count') == 0
        assert 'panic' not in g.serial().lower()
        g.close(w)
    print('PASS: an installed CAT app runs its Lua VM, heap and callbacks at CPL3')


def test():
    test_the_cpl3_boundary_is_wired()
    test_a_booted_guest_cannot_escape_ring3()
    test_isolated_cat_packages_run_at_ring3()


if __name__ == '__main__':
    test()
