"""Lua isolation breakout suite.

An isolated CAT package runs entirely at CPL3: its Lua state, bytecode and heap
live in the app's own address space, and every effect on the system is a
syscall checked against a capability set the app cannot read or write.  This
suite ships the *escape app* (apps/examples/isolation.lua), which tries about
fifty different ways out, and requires that every single one is contained.

It also runs one script twice - isolated and not - and compares the signature
it computes from the `scos` API, because the promise is not just "isolated is
safe" but "isolated behaves like every other CAT package".
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

SC_ISO_ALWAYS = 1
SC_ISO_NEVER = 2


def install(g, app_id, source, title=b'Test App'):
    """Write a .cat package into home/apps and register it."""
    from test_cat import package
    path = 'home/apps/%s.cat' % app_id
    g.save(path, package(source, id=app_id))
    d, s = g.debug, g.scratch
    d.write(s, path.encode() + b'\0')
    assert g.call('lua_app_install', s), 'install refused for ' + app_id
    d.write(s + 128, app_id.encode() + b'\0')
    return s + 128


def wait_for_report(g, path, seconds=150):
    """Poll until the app has recorded every probe (paint adds the last five)."""
    text = b''
    for _ in range(int(seconds / 0.5)):
        g.run(0.5)
        text = g.read(path) or text
        if b'total=' in text and b'contained=' in text:
            total = int(text.split(b'total=')[1].split()[0])
            contained = int(text.split(b'contained=')[1].split()[0])
            breakout = int(text.split(b'breakout=')[1].split()[0])
            if contained + breakout >= total:
                return text
    return text


def test_the_bait_list_is_the_same_in_the_kernel_and_the_app():
    """One list, two readers.

    The decoys live in kernel/desktop/decoys.c and the probes that must be
    refused live in apps/examples/isolation.lua.  If those two lists drift, the
    system grows bait that nothing tests - which is exactly the shape of a
    security hole nobody notices - so the lists are compared here, at build
    time, without booting anything.
    """
    import re
    kernel = (ROOT / 'kernel' / 'desktop' / 'decoys.c').read_text()
    table = kernel.split('decoy_files[] = {', 1)[1].split('};', 1)[0]
    paths = re.findall(r'\{\s*"([^"]+)"\s*\}', table)
    assert paths, 'the kernel decoy table was not found'
    app = (ROOT / 'apps' / 'examples' / 'isolation.lua').read_text()
    listed = re.findall(r'^\s*"([^"]+\.(?:map|key|db|log|txt))",\s*$', app, re.M)
    assert listed == paths, ('the escape app probes %r but the kernel plants %r' % (listed, paths))
    # The shell must be able to show the same list.
    term = (ROOT / 'kernel' / 'desktop' / 'app_terminal.c').read_text()
    assert 'decoys_path(i)' in term and 'decoys_present(i)' in term, \
        'the shell cannot list the planted decoys'
    print('PASS: %d decoy files, one list in the kernel, the shell and the escape app' % len(paths))


def test_the_escape_app_finds_no_way_out():
    from qemu_support import Guest

    source = (ROOT / 'apps' / 'examples' / 'isolation.lua').read_text()
    with Guest('escape') as g:
        ident = install(g, 'isolation', source)
        assert g.call('security_app_isolation_set', ident, SC_ISO_ALWAYS) == 1
        # make sure the host really is the CPL3 one, not a simulation
        w = g.call('wm_open_app', ident, 0)
        assert w, 'the escape app did not open'
        pid = g.call('proc_pid_for_window', w)
        assert pid and g.call('proc_pid_isolated', pid) == 1
        assert g.call('user_pid_mapped', pid, 0x600000040000) == 1

        # The bait planted by the kernel, captured BEFORE the app runs: what the
        # probes must leave untouched.  The paths come from the kernel's own
        # table, not from this file, so a new decoy is tested without editing
        # the suite.
        decoys = []
        for i in range(g.call('decoys_count')):
            path = g.string(g.call('decoys_path', i))
            # decoys_present() is the kernel's own answer and the only one that
            # works for the decoys planted behind the account guard; a plain
            # read of system/security/<anything> is refused, as it should be.
            assert g.call('decoys_present', i) == 1, 'a decoy was never planted: ' + path
            decoys.append((i, path))

        report = wait_for_report(g, 'home/appdata/root/isolation/report.txt')
        assert report, 'the escape app wrote no report'
        text = report.decode('utf-8', 'replace')
        print(text, flush=True)
        assert 'total=' in text and 'contained=' in text and 'breakout=' in text
        assert 'breakout=0' in text, 'an escape probe SUCCEEDED:\n' + text
        assert 'BREAKOUT' not in text, 'an escape probe SUCCEEDED:\n' + text
        total = int(text.split('total=')[1].split()[0])
        contained = int(text.split('contained=')[1].split()[0])
        assert total >= 100 and contained == total, (total, contained)

        # Every planted decoy, and every way of touching it, was refused ...
        for i, path in enumerate(decoys, start=1):
            for op in ('read', 'edit', 'remove', 'exists'):
                tag = 'decoy%d_%s=' % (i, op)
                line = [l for l in text.splitlines() if l.startswith(tag)]
                assert line, 'the escape app never tried %s %s' % (path, op)
                assert 'BREAKOUT' not in line[0], line[0]
        assert 'decoy_rename_over=CONTAINED' in text

        # ... and not one byte of the bait changed while it ran.
        for index, path in decoys:
            assert g.call('decoys_present', index) == 1, \
                'the sandbox CHANGED or DELETED a planted decoy: ' + path
        print('%d planted decoy files were read, edited, removed and renamed at, and none moved'
              % len(decoys), flush=True)
        assert 'api_info_isolation=CONTAINED' in text, \
            'the API did not report the sandbox it is running in'

        # Nothing the app tried may exist outside its own data directory.
        for path in ('escape.txt', 'home/escape.txt', 'home/evil.txt',
                     'home/appdata/root/notepad/steal.txt',
                     'home/appdata/root/isolation/../notepad/steal.txt'):
            assert g.read(path) is None, 'traversal left a file behind: ' + path
        assert g.read('home/appdata/root/isolation/own.txt') is None, \
            'a probe file outlived the app'
        assert g.read('home/appdata/root/isolation/sub/evil.txt') is None

        # The boundary held without faults, and the app still owns its data.
        assert g.call('user_fault_count') == 0
        assert g.call('user_syscall_count') > 0
        assert g.read('home/appdata/root/isolation/report.txt')

        g.call('wm_close_window', w)
        assert g.call('proc_count') == 0, 'process leaked after close'
        assert 'panic' not in g.serial().lower()
    print('PASS: every escape attempt in the isolated Lua app was contained',
          flush=True)


PARITY_APP = r"""
-- Compute a deterministic signature of the scos API surface.  Both hosts must
-- produce exactly the same text.
local out = {}
local function add(k, v) out[#out + 1] = k .. "=" .. tostring(v) end
local function sorted(t)
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    return table.concat(keys, ",")
end
add("version", scos.version)
add("rgb", string.format("%06x", scos.rgb(1, 2, 3)))
add("blend", string.format("%06x", scos.blend(0, 0xffffff, 50)))
add("clamp", scos.clamp(200, 0, 100) .. "/" .. scos.clamp(-5, 0, 100))
add("hit", tostring(scos.hit(3, 4, 0, 0, 10, 10)) .. "/" .. tostring(scos.hit(10, 10, 0, 0, 10, 10)))
add("text_width", scos.text_width("abc"))
add("icon_size", table.concat({scos.icon_size()}, "/"))
add("icons", sorted(scos.icons()))
add("themes", table.concat(scos.themes(), ","))
local th = scos.theme()
add("theme_id", th.id)
add("theme_keys", sorted(th))
add("theme_main", string.format("%06x", th.main))
add("theme_mode", th.mode)
local info = scos.api_info()
add("info", info.version .. "/" .. info.source_limit .. "/" .. info.arena_limit .. "/" ..
            info.data_limit .. "/" .. info.data_files)
add("window_keys", sorted(scos.window()))
add("size", table.concat({scos.size()}, "/"))
add("screen", table.concat({scos.screen()}, "/"))
add("mouse_types", type(scos.mouse()))
add("focused", type(scos.focused()))
add("app_id", scos.app_id())
add("date_keys", sorted(scos.date()))
add("clock", type(scos.time()))
add("memory_limit", select(2, scos.memory()))
add("theme_status", scos.theme_status())
add("files", type(scos.files()))
add("uptime", type(scos.uptime()))
add("now_ms", type(scos.now_ms()))
add("random", tostring(scos.random(1)))
add("storage", sorted(scos.storage()))
add("text_rows", scos.text_rows(80, "two words") .. "/" .. scos.text_rows(80, ""))
add("text_height", scos.text_height())
add("icon_id", tostring(scos.icon_id("notepad")) .. "/" .. tostring(scos.icon_id("nope")))
add("hsv", string.format("%06x", scos.hsv(0, 100, 100)) .. "/" .. string.format("%06x", scos.hsv(210, 0, 50)))
scos.write("sig.txt", table.concat(out, "\n") .. "\n")
return {
    open = function() scos.title("Parity") end,
    paint = function(w, h) scos.clear(scos.theme().win_bg) end,
}
"""


def test_isolated_and_normal_hosts_expose_the_same_api():
    from qemu_support import Guest

    with Guest('parity') as g:
        ident = install(g, 'parity', PARITY_APP)
        signatures = {}
        for mode, label in ((SC_ISO_ALWAYS, 'isolated'), (SC_ISO_NEVER, 'normal')):
            assert g.call('security_app_isolation_set', ident, mode) == 1
            w = g.call('wm_open_app', ident, 0)
            assert w, label + ' launch failed'
            pid = g.call('proc_pid_for_window', w)
            assert pid
            assert g.call('proc_pid_isolated', pid) == (1 if mode == SC_ISO_ALWAYS else 0)
            g.run(0.8)
            data = g.read('home/appdata/root/parity/sig.txt')
            assert data, label + ' run wrote no signature'
            signatures[label] = data.decode()
            g.call('wm_close_window', w)
            g.read('home/appdata/root/parity/sig.txt')
            # clear the file so a stale copy cannot pass the next round
            g.save('home/appdata/root/parity/sig.txt', b'')
            g.run(0.1)
        assert signatures['isolated'] == signatures['normal'], \
            'API signatures differ\n--- isolated ---\n%s\n--- normal ---\n%s' % (
                signatures['isolated'], signatures['normal'])
        print(signatures['isolated'], flush=True)
        assert 'version=2' in signatures['isolated']
        assert 'panic' not in g.serial().lower()
    print('PASS: isolated and kernel-side CAT hosts expose the same scos API',
          flush=True)


SPINNER = """
-- A package that does nothing but burn CPU.  There is no syscall to charge the
-- time against, so only the timer can stop it.
return {
    open = function() while true do end end,
    paint = function(w, h) scos.clear(0) end,
}
"""


def test_a_spinning_package_is_stopped_and_the_desktop_survives():
    from qemu_support import Guest

    with Guest('spin') as g:
        ident = install(g, 'spin', SPINNER)
        assert g.call('security_app_isolation_set', ident, SC_ISO_ALWAYS) == 1
        before = g.call('user_fault_count')
        w = g.call('wm_open_app', ident, 0)      # blocks in the spinning open()
        assert w
        state, message = g.state(w)
        assert state == 1 and 'CPU budget' in message, (state, message)
        assert g.call('user_fault_count') == before
        # The desktop still answers and still starts other apps.
        assert g.call('wm_win_count') >= 1
        d = g.debug
        d.write(g.scratch + 256, b'calendar\0')
        w2 = g.call('wm_open_app', g.scratch + 256, 0)
        assert w2 and w2 != w
        g.run(0.3)
        assert g.state(w2) == (0, ''), g.state(w2)
        assert 'panic' not in g.serial().lower()
    print('PASS: an isolated package that spins without syscalls is stopped by '
          'the timer budget, and the desktop keeps running', flush=True)


NOT_ISOLATED = """
-- The same app, launched by the owner without isolation, must say so instead of
-- asking a normal app to take over the system theme.
return {
    open = function() scos.interval(200) end,
    paint = function(w, h)
        local t = scos.theme()
        scos.clear(t.win_bg)
        local info = scos.api_info()
        scos.text(10, 10, info.isolated and "sandboxed" or "not sandboxed", t.main)
    end,
}
"""


def test_an_unisolated_launch_does_not_probe_or_ask_for_the_theme():
    """The escape app runs its probes only where probes mean something."""
    from qemu_support import Guest

    with Guest('not-isolated') as g:
        ident = install(g, 'nonew', NOT_ISOLATED)
        assert g.call('security_app_isolation_set', ident, SC_ISO_NEVER) == 1
        w = g.call('wm_open_app', ident, 0)
        pid = g.call('proc_pid_for_window', w)
        assert g.call('proc_pid_isolated', pid) == 0
        g.run(0.4)
        # No capability dialog appeared: an unisolated launch never probes.
        assert g.call('wm_dialog_active') == 0

        # And the shipped escape app, run unisolated, writes no report and
        # raises no dialog either.
        source = (ROOT / 'apps' / 'examples' / 'isolation.lua').read_text()
        ident = install(g, 'isolation', source)
        assert g.call('security_app_isolation_set', ident, SC_ISO_NEVER) == 1
        w = g.call('wm_open_app', ident, 0)
        assert w
        g.run(1.0)
        assert g.call('wm_dialog_active') == 0, 'an unisolated launch asked for something'
        assert g.read('home/appdata/root/isolation/report.txt') is None, \
            'an unisolated launch still ran the escape probes'
        assert g.call('user_fault_count') == 0
        assert 'panic' not in g.serial().lower()
    print('PASS: an unisolated launch explains itself instead of probing the '
          'machine it is allowed to change', flush=True)


def test_the_escape_app_is_shipped_as_a_demo():
    """The attack app has to be reachable by the owner, not just by the tests."""
    inc = ROOT / 'build' / 'lua_examples.h'
    if inc.exists():
        assert 'isolation' in inc.read_text(), \
            'isolation.cat is not embedded into /home/demos'
    readme = (ROOT / 'tools' / 'embed_apps.py').read_text()
    assert 'isolation' in readme.lower(), 'the demo README does not mention the app'
    docs = (ROOT / 'docs' / 'USERMODE.md').read_text()
    assert 'isolation.lua' in docs, 'docs/USERMODE.md does not document the escape app'
    print('PASS: the escape app ships as a demo and is documented')


def test():
    test_the_escape_app_finds_no_way_out()
    test_isolated_and_normal_hosts_expose_the_same_api()
    test_a_spinning_package_is_stopped_and_the_desktop_survives()
    test_an_unisolated_launch_does_not_probe_or_ask_for_the_theme()
    test_the_escape_app_is_shipped_as_a_demo()


if __name__ == '__main__':
    test()
