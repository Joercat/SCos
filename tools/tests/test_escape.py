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


def wait_for_report(g, path, seconds=25):
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

        report = wait_for_report(g, 'home/appdata/isolation/report.txt')
        assert report, 'the escape app wrote no report'
        text = report.decode('utf-8', 'replace')
        print(text, flush=True)
        assert 'total=' in text and 'contained=' in text and 'breakout=' in text
        assert 'breakout=0' in text, 'an escape probe SUCCEEDED:\n' + text
        assert 'BREAKOUT' not in text, 'an escape probe SUCCEEDED:\n' + text
        total = int(text.split('total=')[1].split()[0])
        contained = int(text.split('contained=')[1].split()[0])
        assert total >= 40 and contained == total, (total, contained)

        # Nothing the app tried may exist outside its own data directory.
        for path in ('escape.txt', 'home/escape.txt', 'home/evil.txt',
                     'home/appdata/notepad/steal.txt',
                     'home/appdata/isolation/../notepad/steal.txt'):
            assert g.read(path) is None, 'traversal left a file behind: ' + path
        assert g.read('home/appdata/isolation/own.txt') is None, \
            'a probe file outlived the app'
        assert g.read('home/appdata/isolation/sub/evil.txt') is None

        # The boundary held without faults, and the app still owns its data.
        assert g.call('user_fault_count') == 0
        assert g.call('user_syscall_count') > 0
        assert g.read('home/appdata/isolation/report.txt')

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
            data = g.read('home/appdata/parity/sig.txt')
            assert data, label + ' run wrote no signature'
            signatures[label] = data.decode()
            g.call('wm_close_window', w)
            g.read('home/appdata/parity/sig.txt')
            # clear the file so a stale copy cannot pass the next round
            g.save('home/appdata/parity/sig.txt', b'')
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
    test_the_escape_app_is_shipped_as_a_demo()


if __name__ == '__main__':
    test()
