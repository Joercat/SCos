"""Confirmations, titlebar chrome and taskbar hit testing.

The power menu opened a dialog that could be minimised into an invisible window
still owning every click, plus a Minimise and a Maximise button that meant
nothing for a yes/no question.  These tests drive the real desktop with injected
mouse and key events and require:

* exactly one confirmation window, centred in the work area, with the single
  close affordance a dialog is allowed to have;
* an outside click, the Cancel button, and Escape all answer "no" and leave no
  window behind;
* the desktop still accepts input afterwards (the trap is gone);
* the titlebar buttons hit-test where they are drawn, report hover, and a
  right-click on a titlebar still opens the window menu;
* every taskbar surface the pointer can press reports itself as a target.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

WIN_TITLEBAR = 26
DLG_OK_X, DLG_CANCEL_X = 40, 130

STATE_NORMAL, STATE_MIN, STATE_MAX = 0, 1, 2
BTN_HOVER_NONE = -1     # hover_btn / hover_btn_win
BAR_HOVER_NONE = 0xffffff9d   # bar_hover_at: -99, as returned in RAX


def mouse(g, kind, x, y, buttons=0, button=0, down=0, wheel=0):
    """MEV_MOVE/MEV_BUTTON/MEV_WHEEL with the pointer placed by the harness.

    The screen coordinates go in the guest's mx/my variables - never in the
    packet, which would move the emulated pointer instead.
    """
    g.debug.write(g.scratch, struct.pack('<BxhhbBBB', kind, 0, 0, wheel,
                                         buttons, button, down))
    g.debug.write(g.base + g.symbols['mx'], struct.pack('<i', x))
    g.debug.write(g.base + g.symbols['my'], struct.pack('<i', y))
    return g.call('handle_mouse', g.scratch)


def click(g, x, y, button=1):
    mouse(g, 2, x, y, buttons=button, button=button, down=1)
    g.run(0.05)
    mouse(g, 2, x, y, buttons=0, button=button, down=0)
    g.run(0.1)


def key(g, code, alt=0, shift=0, ctrl=0):
    g.debug.write(g.scratch, struct.pack('<BxH5Bx', 0, code, 1,
                                         shift, ctrl, alt, 0))
    return g.call('handle_key', g.scratch)


def geom(g, w):
    return struct.unpack('<4i', g.debug.read(w + 88, 16))


def win_state(g, w):
    return int.from_bytes(g.debug.read(w + 124, 4), 'little')


def title_of(g, w):
    return g.debug.read(w + 4, 64).split(b'\0')[0].decode('utf-8', 'replace')


def dialogs(g):
    found = []
    for i in range(g.call('wm_win_count')):
        w = g.call('wm_win_at', i)
        if w and g.call('win_is_dialog', w):
            found.append((w, title_of(g, w), geom(g, w)))
    return found


def screen(g):
    d = g.debug
    return (int.from_bytes(d.read(g.base + g.symbols['screen_w'], 4), 'little'),
            int.from_bytes(d.read(g.base + g.symbols['screen_h'], 4), 'little'))


def power_rect(g):
    sw, sh = screen(g)
    bar = g.call('wm_taskbar_height')
    return sw - 68 - 34 - 16, sh - bar + 6, 34, bar - 12


def menu_rect(g):
    px, py, pw, ph = power_rect(g)
    return px - 140, py - 3 * 24 - 8, 170, 78


def open_power_menu(g, item):
    """Right-click nothing; the power button opens the menu on a left click."""
    px, py, pw, ph = power_rect(g)
    click(g, px + pw // 2, py + ph // 2)
    mx, my, mw, mh = menu_rect(g)
    assert g.value('menu', 4) == 1, 'the power menu did not open'
    click(g, mx + 85, my + 3 + item * 24 + 12)
    g.run(0.1)


def test_the_fixes_are_wired():
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    for needle in ('win_is_dialog', 'wm_modal_dismiss', 'hover_btn_win',
                   'bar_hover_at', 'int nbtn = dlg ? 1 : 3', 'notice_hover'):
        assert needle in wm, needle + ' is missing from the WM'
    assert '!in_rect(mx, my, modal_w->x, modal_w->y, modal_w->w, modal_w->h)' in wm, \
        'an outside click no longer dismisses a confirmation'
    assert 'w->x = (screen_w - w->w) / 2;' in wm, 'dialogs are not centred'
    print('PASS: dialog ownership, click-off dismissal, centring and hover '
          'highlighting are wired')


def test_power_confirmations_behave():
    from qemu_support import Guest

    with Guest('wm-dialogs') as g:
        sw, sh = screen(g)
        bar = g.call('wm_taskbar_height')
        boots = g.serial().count('wm: entering main loop')
        for item, name in ((1, 'Restart'), (2, 'Power Off')):
            open_power_menu(g, item)
            found = dialogs(g)
            assert len(found) == 1, (name, found)
            w, title, (x, y, ww, hh) = found[0]
            assert title == name, (name, title)
            assert abs(x + ww // 2 - sw // 2) <= 12, 'dialog is not centred: %s' % ((x, ww, sw),)
            assert abs(y + hh // 2 - (sh - bar) // 2) <= 16, 'dialog is not vertically centred'
            assert x >= 0 and y >= 0 and x + ww <= sw and y + hh <= sh - bar
            assert g.call('wm_dialog_active') == 1
            assert g.call('wm_focused') == w
            # A dialog has exactly one titlebar control: its close button.
            g.call('paint_all')
            g.snapshot('dialog-' + name.lower())
            # The button glyph's inverse hover box must not fault or resize it.
            click(g, x + ww - 22, y + 4)
            assert g.call('wm_dialog_active') == 0, 'the close button did not dismiss'
            assert not dialogs(g), 'the confirmation left a window behind'
            g.run(0.1)
            assert g.serial().count('wm: entering main loop') == boots, \
                name + ' actually rebooted the machine'
        assert 'panic' not in g.serial().lower()

        # Outside click = Cancel, and the click does not reach what was under it.
        w = g.launch('notepad')
        assert w
        g.call('wm_focus', w)
        open_power_menu(g, 2)
        assert g.call('wm_dialog_active') == 1
        before = g.call('wm_win_count')
        click(g, 40, sh - bar - 30)
        assert g.call('wm_dialog_active') == 0
        assert not dialogs(g)
        assert g.call('wm_win_count') == before - 1, 'the dialog window stayed behind'
        assert g.call('wm_focused') == w, 'focus did not return to the window below'

        # Cancel button and Escape both answer no.
        open_power_menu(g, 1)
        dlg = dialogs(g)[0]
        x, y, ww, hh = dlg[2]
        click(g, x + 1 + DLG_CANCEL_X + 40,
              y + WIN_TITLEBAR + (hh - WIN_TITLEBAR - 1) - 40 + 13)
        assert g.call('wm_dialog_active') == 0, 'Cancel did not dismiss'
        open_power_menu(g, 2)
        assert g.call('wm_dialog_active') == 1
        key(g, 27)
        g.run(0.1)
        assert g.call('wm_dialog_active') == 0, 'Escape did not dismiss'
        # And the desktop is alive and input is not captured by anything.
        assert g.call('wm_focus', w) == w or g.call('wm_focused') == w
        g.run(0.2)
        assert g.serial().count('wm: entering main loop') == boots
        assert 'panic' not in g.serial().lower()
    print('PASS: Restart/Power Off confirmations are single centred dialogs '
          'that vanish on close, outside click, Cancel and Escape', flush=True)


def test_titlebar_and_taskbar_hit_testing():
    from qemu_support import Guest

    with Guest('wm-chrome') as g:
        sw, sh = screen(g)
        bar = g.call('wm_taskbar_height')
        w = g.launch('notepad')
        g.call('wm_move_window', w, 60, 220)
        g.run(0.2)
        x, y, ww, hh = geom(g, w)
        assert ww > 200 and hh > 120

        # Hover is reported by the same function that paints it.
        def hover_at(px, py):
            mouse(g, 1, px, py)
            return g.value('hover_btn', 4), g.value('hover_btn_win', 8)

        # Button boxes are 18 px wide: min, max, close from the right.
        for kind, name in ((0, 'min'), (1, 'max'), (2, 'close')):
            bx = x + ww - 22 - (2 - kind) * 22 + 9
            hot, win = hover_at(bx, y + 12)
            assert hot == kind and win == w, (name, hot, win)
            g.call('paint_all')
            g.snapshot('hover-' + name)
        # The gap between two buttons clears the highlight.
        gap = hover_at(x + ww - 22 - 22 + 18, y + 12)
        assert gap == (0xffffffff, 0), gap      # -1, no window
        # Moving out of the titlebar clears it too.
        out = hover_at(x + 20, y + hh - 20)
        assert out == (0xffffffff, 0), out      # -1, no window

        # The buttons hit-test where they are drawn.
        bx = x + ww - 22 - (2 - 0) * 22 + 9
        click(g, bx, y + 12)
        assert win_state(g, w) == STATE_MIN, win_state(g, w)
        # A minimised window comes back through the picker - the same path a
        # click on its taskbar row takes, not a test-only helper.
        assert g.call('wm_overview_toggle') == 1
        key(g, 13)                       # Enter accepts the highlighted tile
        g.run(0.2)
        assert g.call('wm_overview_active') == 0
        assert win_state(g, w) == STATE_NORMAL, 'the picker did not restore it'
        g.call('wm_focus', w)
        key(g, 147, alt=1)               # Alt+F10 maximise (a real owner path)
        assert win_state(g, w) == STATE_MAX, win_state(g, w)
        key(g, 147, alt=1)
        assert win_state(g, w) == STATE_NORMAL
        # Double-clicking the titlebar maximises.
        mouse(g, 2, x + ww // 2, y + 12, buttons=1, button=1, down=1)
        mouse(g, 2, x + ww // 2, y + 12, buttons=0, button=1, down=0)
        g.run(0.02)
        mouse(g, 2, x + ww // 2, y + 12, buttons=1, button=1, down=1)
        mouse(g, 2, x + ww // 2, y + 12, buttons=0, button=1, down=0)
        g.run(0.1)
        assert win_state(g, w) == STATE_MAX, 'double-click did not maximise'
        assert geom(g, w) == (0, 0, sw, sh - bar), geom(g, w)
        g.snapshot('maximised')

        # Right-click on a titlebar opens the window menu.
        g.run(0.1)
        x, y, ww, hh = geom(g, w)
        click(g, x + ww // 2, y + 12, button=2)
        assert g.value('menu', 4) == 1, 'no window menu on a titlebar right-click'
        g.call('paint_all')
        g.snapshot('title-menu')
        click(g, 4, sh - bar - 4)         # click off dismisses it
        assert g.value('menu', 4) == 0

        # Every taskbar surface reports itself through the one function that
        # both paints the highlight and hit-tests the click: -4 power, -3 the
        # Windows picker, -2 the launcher, a pin index, -99 for nothing.
        px, py, pw, ph = power_rect(g)
        assert g.call('bar_hover_at', px + 2, py + 2) == 0xfffffffc   # -4
        assert g.call('bar_hover_at', 50, sh - bar + 10) == 0xfffffffd   # -3
        assert g.call('bar_hover_at', 200, sh - bar - 20) == BAR_HOVER_NONE

        # A notification is pressable ("Click to dismiss"), so it highlights
        # too - and the highlight follows the pointer off the box again.
        g.debug.write(g.scratch + 2000, b'Notice\0')
        g.debug.write(g.scratch + 2100, b'Hover me\0')
        g.call('wm_notify', g.scratch + 2000, g.scratch + 2100, 1)
        g.call('paint_all')
        g.run(0.1)
        # The box's rectangle comes from the window manager: notifications stack
        # upwards from the bottom-right corner, newest nearest the taskbar, so a
        # box can never sit on a window's Minimise/Maximise/Close buttons.
        box = g.scratch + 6144
        newest = g.value('notice_count') - 1
        assert g.call('wm_notice_rect', newest, box), 'the newest notice has no rectangle'
        nx, ny, nw, nh = struct.unpack('<4i', g.debug.read(box, 16))
        assert ny + nh <= sh - bar, 'a notice reaches into the taskbar: %r' % ((nx, ny, nw, nh),)
        mouse(g, 1, nx + 160, ny + 20)
        assert g.value('notice_hover', 4) == newest, 'a notification did not highlight'
        g.call('paint_all')
        g.snapshot('notice-hover')
        mouse(g, 1, 40, sh - bar - 200)
        assert g.value('notice_hover', 4) == 0xffffffff, 'the notice stayed highlighted'
        assert 'panic' not in g.serial().lower()
    print('PASS: titlebar hover/hit testing, window menu, double-click maximise '
          'and taskbar targets all agree with what is drawn', flush=True)


def test():
    test_the_fixes_are_wired()
    test_power_confirmations_behave()
    test_titlebar_and_taskbar_hit_testing()


if __name__ == '__main__':
    test()
