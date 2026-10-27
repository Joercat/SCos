"""scoswm-arch -> SCos native WM port.

The Linux WM (Joercat/scoswm-arch) added an interaction layer on top of the
SCos desktop look: an MRU visual window overview, global hotkeys, snap
layouts, live desktop preferences and a confirmed power menu.  This suite
checks the native port of those features, with real key events and real
window surfaces in QEMU.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

WMP_BAR_HEIGHT, WMP_ICON_SIZE, WMP_GRID = 0, 1, 2
WMP_HOTKEYS, WMP_OVERVIEW, WMP_SNAP = 3, 4, 5
KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT = 128, 129, 130, 131
KEY_F4, KEY_F9, KEY_F10 = 141, 146, 147


def key(g, code, alt=0, shift=0, ctrl=0, sup=0):
    """Inject a struct key_event directly into the WM's key handler."""
    g.debug.write(g.scratch, struct.pack('<BxH5Bx', 0, code, 1,
                                         shift, ctrl, alt, sup))
    return g.call('handle_key', g.scratch)


def mouse(g, kind, x, y, buttons=0, button=0, down=0, wheel=0):
    g.debug.write(g.scratch, struct.pack('<BxhhbBBB', kind, x, y, wheel,
                                         buttons, button, down))
    return g.call('handle_mouse', g.scratch)


def test_the_wm_port_is_wired():
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    for needle in ('ov_rebuild', 'ov_paint', 'wm_snap_window',
                   'wm_cycle_focus', 'wm_toggle_maximize', 'wm_confirm',
                   'security_ui_lock', 'WMP_HOTKEYS', 'wm_overview_toggle'):
        assert needle in wm, needle + ' is missing from the native WM'
    prefs = (ROOT / 'kernel/desktop/wm_prefs.c').read_text()
    assert 'home/desktop.cfg' in prefs and 'wm_desktop_pref_set' in prefs
    kbd = (ROOT / 'kernel/desktop/kbd.c').read_text()
    assert 'super_on' in kbd and 'KEY_LSUPER' in kbd
    settings = (ROOT / 'kernel/desktop/app_settings.c').read_text()
    assert 'Window manager' in settings and 'wm_desktop_pref_set' in settings
    header = (ROOT / 'kernel/include/scos.h').read_text()
    assert 'super' in header and 'WMP_BAR_HEIGHT' in header
    assert 'wm_confirm' in header
    print('PASS: MRU overview, hotkeys, snap, prefs and confirmed power '
          'menu are wired into the native compositor')


def test_a_booted_guest_has_the_new_wm():
    from qemu_support import Guest  # noqa: E402

    with Guest('wm-port') as g:
        d, s = g.debug, g.scratch
        # Defaults reproduce the original SCos desktop exactly.
        assert g.call('wm_taskbar_height') == 40
        for k, expected in ((WMP_ICON_SIZE, 24), (WMP_GRID, 1),
                            (WMP_HOTKEYS, 1), (WMP_OVERVIEW, 1), (WMP_SNAP, 1)):
            assert g.call('wm_desktop_pref', k) == expected, (k, expected)

        # ---- MRU visual window overview -----------------------------------
        w1 = g.launch('notepad')
        w2 = g.launch('about')
        w3 = g.launch('calendar')
        assert w1 and w2 and w3
        g.call('wm_focus', w1)            # most recently used
        g.run(0.2)
        assert g.call('wm_overview_active') == 0
        assert g.call('wm_overview_toggle') == 1
        assert g.call('wm_overview_count') == 3
        first_id = int.from_bytes(d.read(w1, 4), 'little')
        tiles = []
        for i in range(3):
            d.write(s + 4096, b'\0' * 24)
            assert g.call('wm_overview_tile', i, s + 4096, s + 4100,
                          s + 4104, s + 4108, s + 4112) == 1
            x, y, w, h, wid = struct.unpack('<5i', d.read(s + 4096, 20))
            assert w > 0 and h > 0, 'degenerate overview tile'
            assert x >= 24 and y >= 24, 'tile escapes the work area'
            screen_h = int.from_bytes(d.read(g.base + g.symbols['screen_h'], 4), 'little')
            assert y + h <= screen_h - g.call('wm_taskbar_height')
            tiles.append((x, y, w, h, wid))
        assert tiles[0][4] == first_id, 'tile 0 is not the most recent window'
        # No two tiles overlap.
        for i in range(3):
            for j in range(i + 1, 3):
                a, b = tiles[i], tiles[j]
                assert not (a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and
                            a[1] < b[1] + b[3] and b[1] < a[1] + a[3]), \
                    'overview tiles overlap'
        g.call('paint_all')
        g.snapshot('wm-overview')
        # Keyboard navigation and Escape.
        before = g.call('wm_overview_hover')
        key(g, KEY_RIGHT)
        assert g.call('wm_overview_hover') != before
        key(g, 27)
        assert g.call('wm_overview_active') == 0
        # Right-click on "Windows N" toggles it back.
        sh = g.call('wm_taskbar_height')
        hy = int.from_bytes(d.read(g.base + g.symbols['screen_h'], 4), 'little') - sh + 8
        d.write(g.base + g.symbols['mx'], struct.pack('<i', 60))
        d.write(g.base + g.symbols['my'], struct.pack('<i', hy))
        mouse(g, 2, 0, 0, buttons=2, button=2, down=1)
        assert g.call('wm_overview_active') == 1
        key(g, 27)

        # ---- global hotkeys ------------------------------------------------
        g.call('wm_focus', w1)
        focused = g.value('focused_w', 8)
        key(g, 9, alt=1)                 # Alt+Tab
        assert g.value('focused_w', 8) != focused, 'Alt+Tab did not cycle'
        key(g, 9, alt=1, shift=1)        # Alt+Shift+Tab
        assert g.value('focused_w', 8) == focused
        key(g, KEY_F9, alt=1)            # Alt+F9 minimise
        assert int.from_bytes(d.read(focused + 124, 4), 'little') == 1
        d.write(focused + 124, struct.pack('<i', 0))   # restore for the next checks
        g.call('wm_focus', focused)
        key(g, KEY_F10, alt=1)           # Alt+F10 maximise
        assert int.from_bytes(d.read(focused + 124, 4), 'little') == 2
        key(g, KEY_F10, alt=1)
        assert int.from_bytes(d.read(focused + 124, 4), 'little') == 0
        key(g, ord('d'), sup=1)          # Super+D -> launcher
        assert g.value('launch', 4) == 1
        key(g, ord('d'), sup=1)
        assert g.value('launch', 4) == 0
        # Super+arrows drive the snap layouts.
        key(g, KEY_LEFT, sup=1)
        screen_w = int.from_bytes(d.read(g.base + g.symbols['screen_w'], 4), 'little')
        assert int.from_bytes(d.read(focused + 88, 4), 'little') == 0
        assert int.from_bytes(d.read(focused + 96, 4), 'little') == screen_w // 2
        key(g, KEY_DOWN, sup=1)          # restore
        assert int.from_bytes(d.read(focused + 96, 4), 'little') != screen_w // 2
        key(g, KEY_UP, sup=1)            # maximise
        assert int.from_bytes(d.read(focused + 124, 4), 'little') == 2
        key(g, KEY_F10, alt=1)           # back to normal

        # ---- live, persisted desktop preferences ---------------------------
        assert g.call('wm_desktop_pref_set', WMP_BAR_HEIGHT, 48) == 48
        assert g.call('wm_taskbar_height') == 48
        cfg = g.read('home/desktop.cfg') or b''
        assert b'bar_height = 48' in cfg, cfg
        assert g.call('wm_desktop_pref_set', WMP_ICON_SIZE, 32) == 32
        assert g.call('wm_desktop_pref', WMP_ICON_SIZE) == 32
        g.call('paint_all')              # scaled icons must not fault
        # Hotkeys/overview/snap are real owner switches, not hard-coded on.
        g.call('wm_desktop_pref_set', WMP_HOTKEYS, 0)
        f = g.value('focused_w', 8)
        key(g, 9, alt=1)
        assert g.value('focused_w', 8) == f, 'hotkeys fired while switched off'
        g.call('wm_desktop_pref_set', WMP_HOTKEYS, 1)
        g.call('wm_desktop_pref_set', WMP_OVERVIEW, 0)
        assert g.call('wm_overview_toggle') == 0
        g.call('wm_desktop_pref_set', WMP_OVERVIEW, 1)
        g.call('wm_desktop_pref_set', WMP_SNAP, 0)
        assert g.call('wm_snap_window', focused, 1) == 0
        g.call('wm_desktop_pref_set', WMP_SNAP, 1)
        # A hand-edited config is clamped, never trusted.
        g.save('home/desktop.cfg', b'bar_height = 9999\ndesktop_icon_size = 77\nhotkeys = 1\n')
        g.call('wm_prefs_init')
        assert g.call('wm_taskbar_height') <= 96
        assert g.call('wm_desktop_pref', WMP_ICON_SIZE) in (24, 32, 48)
        # Restore the shipping defaults for the rest of the suite.
        for k, v in ((WMP_BAR_HEIGHT, 40), (WMP_ICON_SIZE, 24), (WMP_GRID, 1),
                     (WMP_HOTKEYS, 1), (WMP_OVERVIEW, 1), (WMP_SNAP, 1)):
            g.call('wm_desktop_pref_set', k, v)
        g.call('wm_reflow_windows')

        # ---- power menu: Cancel is the safe keyboard default ---------------
        assert 'security_ui_lock' in (ROOT / 'kernel/desktop/wm.c').read_text()
        g.call('power_menu_cb', 1, 0)     # Restart... must confirm first
        assert g.call('wm_dialog_active') == 1
        boots = g.serial().count('wm: entering main loop')
        g.type('\n')                      # Enter must CANCEL, not reboot
        g.run(0.2)
        assert g.call('wm_dialog_active') == 0
        assert g.serial().count('wm: entering main loop') == boots, \
            'Enter confirmed a destructive power action'
        assert 'panic' not in g.serial().lower()
        print('PASS: MRU live-preview overview, Alt/Super hotkeys, snap '
              'layouts, live persisted prefs and Cancel-default power menu')


def test():
    test_the_wm_port_is_wired()
    test_a_booted_guest_has_the_new_wm()


if __name__ == '__main__':
    test()
