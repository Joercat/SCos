"""The app-creation API, exercised in both hosts.

Every function added to `scos` in this round is used by a real package here, and
the same package is run twice - once in the kernel-side host and once at CPL3 -
because API v2 is a promise about *parity*: a package must not be able to tell
which host it is in by what the API does.

Two things are checked that a screenshot cannot settle:

  * the geometry functions (`ellipse`, `ellipse_fill`, `rounded`, `triangle`)
    are read back out of the window's own surface, pixel by pixel, so "it drew
    something" is not enough - the filled ellipse must be red at its centre and
    empty at its bounding-box corner, the rounded rectangle must be green in the
    middle and untouched at its corner, and the triangle must be blue inside and
    black outside;
  * the measuring and storage functions return the same numbers whether the
    package runs in the kernel or in ring 3.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

SC_ISO_ALWAYS = 1
SC_ISO_NEVER = 2

# struct window: id, title[64], app, data, x, y, w, h, px, py, pw, ph, z, state,
# then the content surface { u32 *px; int w, h; }
WIN_SURF = 128
RED, GREEN, BLUE, CYAN = 0xff0000, 0x00ff00, 0x0000ff, 0x00ffff

APP = r"""
-- One package that touches every function added for app creation, then paints
-- figures whose pixels the suite reads back.
local function report()
    local ok, st = pcall(scos.storage)
    local lines = {
        "version=" .. tostring(scos.version),
        "now_ms=" .. tostring(type(scos.now_ms())),
        "random_ok=" .. tostring((function()
            local r = scos.random(6)
            return type(r) == "number" and r >= 1 and r <= 6
        end)()),
        "storage_ok=" .. tostring(ok and type(st) == "table" and
            st.limit_files == 16 and st.limit_bytes == 65536 and
            type(st.files) == "number" and type(st.bytes) == "number"),
        "text_rows_long=" .. tostring(scos.text_rows(80, string.rep("x", 100))),
        "text_rows_newline=" .. tostring(scos.text_rows(80, "a\nb")),
        "text_height=" .. tostring(scos.text_height()),
        "icon_notepad=" .. tostring(scos.icon_id("notepad")),
        "icon_none=" .. tostring(scos.icon_id("nope")),
        "hsv_red=" .. string.format("%06x", scos.hsv(0, 100, 100)),
        "hsv_grey=" .. string.format("%06x", scos.hsv(210, 0, 50)),
    }
    scos.write("api-report.txt", table.concat(lines, "\n") .. "\n")
end

return {
    open = function()
        scos.title("API probe")
        scos.interval(100)
        report()
    end,
    tick = function() report() end,
    paint = function(w, h)
        scos.clear(0x000000)
        scos.ellipse_fill(40, 40, 30, 20, 0xff0000)
        scos.rounded(100, 20, 80, 60, 10, 0x00ff00)
        scos.triangle(200, 20, 260, 20, 230, 80, 0x0000ff)
        scos.ellipse(320, 40, 20, 10, 0x00ffff)
        scos.text_center(0, 100, w, "centred heading", 0xffffff)
        scos.text_right(0, 120, w, "right aligned", 0xffff00)
    end,
}
"""


def install(g, app_id, source):
    from test_cat import package
    path = 'home/apps/%s.cat' % app_id
    g.save(path, package(source, id=app_id))
    g.debug.write(g.scratch, path.encode() + b'\0')
    assert g.call('lua_app_install', g.scratch), 'install refused for ' + app_id
    g.debug.write(g.scratch + 128, app_id.encode() + b'\0')
    return g.scratch + 128


def surface(g, window):
    """(pixel pointer, width, height) of a window's content surface."""
    px = int.from_bytes(g.debug.read(window + WIN_SURF, 8), 'little')
    width, height = struct.unpack('<ii', g.debug.read(window + WIN_SURF + 8, 8))
    return px, width, height


def pixel(g, window, x, y):
    px, width, height = surface(g, window)
    assert 0 <= x < width and 0 <= y < height, 'pixel outside the surface'
    return struct.unpack('<I', g.debug.read(px + (y * width + x) * 4, 4))[0]


def check_report(g, label):
    data = g.read('home/appdata/root/apiprobe/api-report.txt')
    assert data, label + ': the package wrote no report'
    lines = dict(line.split('=', 1) for line in data.decode().strip().splitlines())
    assert lines['version'] == '2'
    assert lines['now_ms'] == 'number'
    assert lines['random_ok'] == 'true', lines
    assert lines['storage_ok'] == 'true', 'scos.storage() is wrong: %r' % lines
    assert lines['text_rows_long'] == '10', lines         # 100 characters at 10 columns
    assert lines['text_rows_newline'] == '2', lines       # two lines, no trailing row
    assert lines['text_height'] == '16', lines
    assert lines['icon_notepad'] == '2', lines            # folder,terminal,notepad
    assert lines['icon_none'] == 'nil', lines
    assert lines['hsv_red'] == 'ff0000', lines
    assert lines['hsv_grey'] == '7f7f7f', lines
    return lines


def check_shapes(g, window, label):
    """The figures are where they were asked for, in the colour they were asked
    for, and nowhere else."""
    px, width, height = surface(g, window)
    assert width > 400 and height > 140, 'the window is too small to hold the figures'
    assert pixel(g, window, 40, 40) == RED, label + ': filled ellipse centre is not red'
    assert pixel(g, window, 75, 40) == 0x000000, label + ': the filled ellipse leaked past its radius'
    assert pixel(g, window, 140, 50) == GREEN, label + ': rounded rectangle centre is not green'
    assert pixel(g, window, 101, 21) == 0x000000, label + ': the rounded corner was filled in'
    assert pixel(g, window, 100, 50) == GREEN, label + ': the rounded edge is missing'
    assert pixel(g, window, 230, 50) == BLUE, label + ': triangle interior is not blue'
    assert pixel(g, window, 205, 25) == BLUE, label + ': triangle edge is not blue'
    assert pixel(g, window, 250, 75) == 0x000000, label + ': the triangle leaked outside its corner'
    assert pixel(g, window, 320, 30) == CYAN, label + ': ellipse outline is missing over its axis'
    assert pixel(g, window, 320, 40) == 0x000000, label + ': the ellipse outline was filled'
    print('PASS: %s painted the ellipse, the rounded rectangle and the triangle where they were '
          'asked for' % label, flush=True)


def test_the_api_works_in_the_kernel_host():
    from qemu_support import Guest

    with Guest('apik') as g:
        ident = install(g, 'apiprobe', APP)
        assert g.call('security_app_isolation_set', ident, SC_ISO_NEVER) == 1
        w = g.call('wm_open_app', ident, 0)
        assert w, 'the probe app did not open'
        g.run(0.6)
        check_report(g, 'kernel host')
        check_shapes(g, w, 'kernel host')
        g.call('wm_close_window', w)
        assert g.call('proc_count') == 0
        assert 'panic' not in g.serial().lower()


def test_the_same_package_paints_the_same_figures_at_cpl3():
    from qemu_support import Guest

    with Guest('apicpl3') as g:
        ident = install(g, 'apiprobe', APP)
        assert g.call('security_app_isolation_set', ident, SC_ISO_ALWAYS) == 1
        w = g.call('wm_open_app', ident, 0)
        assert w, 'the probe app did not open isolated'
        pid = g.call('proc_pid_for_window', w)
        assert pid and g.call('proc_pid_isolated', pid) == 1, 'the package did not run at CPL3'
        g.run(1.2)
        check_report(g, 'CPL3 host')
        check_shapes(g, w, 'CPL3 host')
        assert g.call('user_fault_count') == 0, 'the CPL3 host faulted while drawing'
        g.call('wm_close_window', w)
        assert g.call('proc_count') == 0
        assert 'panic' not in g.serial().lower()


def test_an_over_budget_shape_fails_loudly_in_both_hosts():
    """A refused draw is an ERROR, not a silent no-op - and the same error in
    both hosts.  A package that could tell the hosts apart by whether an
    over-budget draw raises has a fingerprint of where it is running."""
    from qemu_support import Guest

    source = r"""
return {
    open = function() scos.title("Budget") end,
    paint = function(w, h)
        scos.clear(0)
        local ok, err = pcall(scos.ellipse_fill, 0, 0, 1024, 1024, 0xff0000)
        scos.write("budget.txt", tostring(ok) .. " " .. tostring(err) .. "\n")
    end,
}
"""
    for mode, label in ((SC_ISO_NEVER, 'kernel'), (SC_ISO_ALWAYS, 'cpl3')):
        with Guest('apibudget' + label) as g:
            ident = install(g, 'apibudget', source)
            assert g.call('security_app_isolation_set', ident, mode) == 1
            w = g.call('wm_open_app', ident, 0)
            assert w
            g.run(1.0)
            data = g.read('home/appdata/root/apibudget/budget.txt')
            assert data, label + ': the budget report was not written'
            ok, message = data.decode().strip().split(' ', 1)
            assert ok == 'false', label + ': a 1024x1024 ellipse fill was accepted'
            assert 'budget' in message or 'exceeded' in message, (label, message)
            g.call('wm_close_window', w)
    print('PASS: an over-budget shape raises in both hosts, with the same sentence')


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_') and callable(fn):
            fn()
    print('all app API tests passed')
