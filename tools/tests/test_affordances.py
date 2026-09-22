"""Per-surface desktop affordances, proven by behaviour instead of by presence.

The complaint this suite exists to answer is that the icon grid and File Explorer kept the look of the
old "Apps" hub but lost the gestures people actually use - double-clicking, dragging things onto other
things, the keyboard.  A test that clicks once and expects a window, or that only reads a menu string
back, proves nothing about a gesture, so every case here drives the real input entry points
(handle_mouse, handle_key, an application's own mouse callback) and then asserts what changed in the
world: a window that opened, an icon that moved a cell over, a shortcut whose name survived into the
desktop record on disk, a file that is now under a different directory.
"""
import ctypes as C, struct
from qemu_support import Guest, RESULTS
from test_compositor import event, pointer

# Geometry the kernel and this file have to agree on, because the gestures are defined in pixels.
WIN_TITLEBAR, LIST_Y, ROW_H, TASKBAR_H = 26, 44, 24, 40
CELL_X, CELL_Y, CELL_DX, CELL_DY, CELL_W, CELL_H = 16, 16, 88, 96, 80, 88


class Files(C.Structure):
    """Mirror of struct files in app_files.c: the row list, the double-click pair, the scroll offset.
    Read through this rather than guessed at, so a drifted field shows up as a wrong offset here
    instead of as a confusing missing row somewhere else."""
    _fields_ = [('path', C.c_char * 256), ('names', (C.c_char * 48) * 64), ('n', C.c_int),
                ('hover_row', C.c_int), ('hover_btn', C.c_int), ('synth', C.c_ubyte * 64),
                ('notice', C.c_int), ('last_tick', C.c_uint64), ('last_row', C.c_int),
                ('targets', (C.c_char * 192) * 64), ('scroll', C.c_int)]


def stringarg(g, text, offset=0):
    g.debug.write(g.scratch + offset, text.encode() + b'\0')
    return g.scratch + offset


def callstr(g, name, text, offset=256):
    return g.call(name, stringarg(g, text, offset))


def signed(v):
    """The debugger hands back a register's low 32 bits, so a kernel -1 arrives as 0xFFFFFFFF."""
    return v - 2 ** 32 if v & (1 << 31) else v


def setv(g, name, value):
    g.debug.write(g.base + g.symbols[name], struct.pack('<i', value))


def press(g, x, y, button=1):
    pointer(g, x, y)
    event(g, 2, button=button, down=1, buttons=button)


def release(g, x, y, button=1):
    pointer(g, x, y)
    event(g, 2, button=button, down=0, buttons=0)


def click(g, x, y, button=1):
    press(g, x, y, button)
    release(g, x, y, button)


def move(g, x, y, buttons=1):
    pointer(g, x, y)
    event(g, 1, buttons=buttons)


def keyname(g, *keys):
    """A real keystroke in through the emulator's PS/2 port, so the scancode table, kbd_poll and the
    window manager's own handler are all part of what is being tested - the same path the user's
    keyboard takes.  The VM has to be running for the guest to read the queued make code."""
    g.debug.send('c')                     # the guest reads the controller only while it runs
    g.press(*keys)
    g.pause()
    g.run(.3)


def app_mouse(g, w, x, y, kind=2, down=1, buttons=1, button=1):
    """Drive one window's mouse callback the way the WM does, with the position in that window's own
    client coordinates."""
    # '<BxhhbBBB': the pad byte is struct mouse_event's alignment hole after `type`, and without it
    # every field from `wheel` on lands one byte early - a press arrives read as a release.
    g.debug.write(g.scratch, struct.pack('<BxhhbBBB', kind, 0, 0, 0, buttons, button, down))
    g.invoke(g.ptr(g.ptr(w + 72) + 72), w, g.scratch, x, y)


def menu_point(g, i):
    """Centre of row i of whichever menu the window manager has raised."""
    a = g.base + g.symbols['menu']
    active, x, y = struct.unpack('<3i', g.debug.read(a, 12))
    assert active, 'no menu is raised'
    return x + 20, y + 3 + i * 24 + 12


def desk_count(g):
    return g.call('wm_desk_vis_count')


def desk_item(g, i):
    ab, pb, lb = g.scratch + 4096, g.scratch + 4224, g.scratch + 4608
    assert g.call('wm_desk_vis_get', i, ab, pb, lb, g.scratch + 5120) == 1
    return (g.string(ab, 128), g.string(pb, 384), g.string(lb, 96),
            struct.unpack('<i', g.debug.read(g.scratch + 5120, 4))[0])


def desk_find(g, app=None, path=None, kind=None):
    for i in range(desk_count(g)):
        a, p, l, k = desk_item(g, i)
        if app is not None and a != app: continue
        if path is not None and p != path: continue
        if kind is not None and k != kind: continue
        return i
    return -1


def icon_center(g, i):
    g.call('wm_desk_pos', i, g.scratch + 5120, g.scratch + 5124)
    gx, gy = struct.unpack('<2i', g.debug.read(g.scratch + 5120, 8))
    return CELL_X + gx * CELL_DX + CELL_W // 2, CELL_Y + gy * CELL_DY + 40, gx, gy


def files_of(g, w):
    return Files.from_buffer_copy(g.debug.read(g.ptr(w + 80), C.sizeof(Files)))


def row_y(g, row):
    return LIST_Y + 4 + row * ROW_H + ROW_H // 2


def row_of(g, w, name):
    f = files_of(g, w)
    for i in range(f.n):
        if bytes(f.names[i]).split(b'\0')[0] == name.encode():
            return i
    return -1


def close_others(g, w):
    """Leave exactly one window up.  The gestures below are aimed at a particular window's client
    area, and a window the previous case left open would simply be the thing under the cursor."""
    for _ in range(8):
        n = g.call('wm_win_count')
        if n <= 1:
            break
        other = next(i for i in range(n) if g.call('wm_win_at', i) != w)
        g.close(g.call('wm_win_at', other))
    assert g.call('wm_win_count') <= 1


def files_refresh(g, w):
    g.call('files_load', g.ptr(w + 80))
    g.call('wm_redraw', w)


def bar_slot(g, app_id, index=False):
    """Screen centre of the bar slot that carries an application, found through the pin table rather
    than assumed to be the first one."""
    pins = g.base + g.symbols['task_pins']
    for i in range(g.value('pin_count')):
        if g.string(pins + i * 32, 32) == app_id:
            return (168 + i * 40 + 17, g.value('screen_h') - TASKBAR_H + 6 + (TASKBAR_H - 12) // 2)
    raise AssertionError('%s is not pinned to the bar' % app_id)


def test():
    with Guest('affordances') as g:
        sw, sh = g.value('screen_w'), g.value('screen_h')
        g.call('paint_all')
        # Bottom-right is the one stretch of wallpaper every screen size has free of both the icon grid
        # and the boot notification, which stays up until it is dismissed.
        clear = (sw - 24, sh - 56)

        # ------------------------------------------------------ double-click is what opens an icon
        about = desk_find(g, app='about')
        assert about >= 0, 'the About icon is not on the desktop'
        x, y, gx, gy = icon_center(g, about)
        assert g.call('wm_app_running', stringarg(g, 'about')) == 0
        click(g, x, y)
        assert g.call('wm_app_running', stringarg(g, 'about')) == 0, 'a single click launched About'
        assert g.value('desk_sel') == (1 << about), 'the press did not select the icon'
        press(g, x, y)
        assert g.call('wm_app_running', stringarg(g, 'about')) == 1, 'double-clicking did not open About'
        release(g, x, y)
        g.close(g.call('wm_win_at', 0))
        assert g.call('wm_win_count') == 0

        # ------------------------------------------------------ Enter opens what is selected
        g.run(.6)                       # let the pair above age out of the double-click window
        click(g, x, y)
        assert g.call('wm_app_running', stringarg(g, 'about')) == 0
        keyname(g, 'ret')
        assert g.call('wm_app_running', stringarg(g, 'about')) == 1, 'Enter did not open the selection'
        g.close(g.call('wm_win_at', 0))

        # ------------------------------------------------------ rubber band, Delete, and back again
        setv(g, 'desk_sel', 0)
        press(g, *clear)
        move(g, 4, 4, buttons=1)
        release(g, 4, 4)
        assert g.value('desk_sel') != 0, 'the rubber band selected nothing'
        total = desk_count(g)
        keyname(g, 'delete')
        assert desk_count(g) == 0, 'Delete left icons on the desktop'
        assert g.call('wm_desk_app_state', stringarg(g, 'about')) == 1, 'About was deleted, not hidden'
        assert g.call('wm_win_count') == 0, 'hiding icons touched someone else\'s windows'
        click(g, *clear, button=2)                    # the wallpaper's own menu, opened on the press
        mx, my = menu_point(g, 0)                     # "Restore hidden icons"
        click(g, mx, my)
        assert desk_count(g) == total, 'restoring brought nothing back'
        about = desk_find(g, app='about')
        x, y, gx, gy = icon_center(g, about)

        # ------------------------------------------------------ dragging an icon to another cell
        before = icon_center(g, about)
        press(g, before[0], before[1])
        move(g, before[0] + 190, before[1] + 200, buttons=1)
        release(g, before[0] + 190, before[1] + 200)
        after = icon_center(g, about)
        assert after[2] > before[2] and after[3] > before[3], \
            'dragging the icon did not move it on the grid: %r -> %r' % (before[2:], after[2:])
        record = g.read('system/desktop.json')
        assert record and b'SCOSDESK1' in record, 'the new position was not written to the desktop record'

        # ------------------------------------------------------ a shortcut's menu lines
        g.call('vfs_write', stringarg(g, 'home/gesture.txt'), stringarg(g, 'kept', 256), 4)
        g.call('wm_desktop_pin_file', stringarg(g, 'home/gesture.txt'))
        sc = desk_find(g, kind=1, path='home/gesture.txt')
        assert sc >= 0, 'the shortcut is not on the desktop'
        # A shortcut to a file carries the two lines its own row has in File Explorer.  An application
        # icon carries neither: its name and its one handler belong to the application.
        assert g.call('wm_desk_action_index', sc, stringarg(g, 'Open with...', 256)) >= 0
        assert g.call('wm_desk_action_index', sc, stringarg(g, 'Rename shortcut', 256)) >= 0
        # -1 comes back from the debugger as the register's low 32 bits, so the negatives are read
        # through signed() the way every other kernel -1 is in these suites.
        assert signed(g.call('wm_desk_action_index', about, stringarg(g, 'Open with...', 256))) == -1
        assert signed(g.call('wm_desk_action_index', about, stringarg(g, 'Rename shortcut', 256))) == -1
        # The lines the gesture offers are the lines the menu really shows, in the same order.
        sx, sy, _, _ = icon_center(g, sc)
        # A shortcut dropped into a cell that was just vacated sits exactly where the last press landed,
        # so the pair has to be aged out before this click is a fair test of a single press.
        g.run(.6)
        click(g, sx, sy)                              # a press selects; only a pair would have opened it
        assert g.call('wm_win_count') == 0
        assert g.value('desk_sel') & (1 << sc), 'the shortcut was not selected by the click'

        # ------------------------------------------------------ F2 renames the shortcut
        # The click above is the selection; pressing it again now would be read as the second half of a
        # double-click, which is the correct answer to two presses and the wrong one to this test.
        keyname(g, 'f2')
        assert g.call('wm_dialog_active'), 'F2 did not raise the rename dialog'
        g.type('renamed note')
        keyname(g, 'ret')         # submitted the same way it was typed: the guest has to be running
        assert not g.call('wm_dialog_active'), 'the rename dialog stayed up'
        assert desk_item(g, sc)[2] == 'renamed note', desk_item(g, sc)
        assert b'renamed note' in g.read('system/desktop.json'), 'the new name was not persisted'
        assert desk_item(g, sc)[1] == 'home/gesture.txt', 'renaming changed what the shortcut opens'

        # ------------------------------------------------------ open with..., from the icon
        line = g.call('wm_desk_action_index', sc, stringarg(g, 'Open with...', 256))
        before_windows = g.call('wm_win_count')
        assert g.call('wm_desk_invoke', sc, line) == 0
        first = g.debug.read(g.base + g.symbols['desk_openwith_ids'], 32).split(b'\0')[0].decode()
        ox, oy = menu_point(g, 0)
        click(g, ox, oy)
        assert g.call('wm_win_count') == before_windows + 1, 'the open-with choice opened no window'
        assert g.call('wm_app_running', stringarg(g, first)) == 1, 'the chosen application refused the file'
        while g.call('wm_win_count'):
            g.close(g.call('wm_win_at', 0))

        # ------------------------------------------------------ File Explorer: rows open on a pair
        w = g.call('wm_open_app', stringarg(g, 'files'), stringarg(g, 'home', 256))
        assert w
        g.run()
        g.call('wm_move_window', w, 120, 80)
        files_refresh(g, w)
        f = files_of(g, w)
        assert f.n > 0 and f.notice == 0, 'File Explorer is not showing home'
        close_others(g, w)
        row = row_of(g, w, 'gesture.txt')
        assert row >= 0, 'the file is not listed'
        app_mouse(g, w, 120, row_y(g, row))
        app_mouse(g, w, 120, row_y(g, row), down=0, buttons=0)
        assert g.call('wm_win_count') == 1, 'a single press on a row opened something'
        app_mouse(g, w, 120, row_y(g, row))
        assert g.call('wm_win_count') == 2, 'double-clicking the row did not open the file'
        g.close(g.call('wm_win_at', 1))

        # ------------------------------------------------------ drag a row onto the wallpaper
        app_mouse(g, w, 120, row_y(g, row))
        app_mouse(g, w, 180, row_y(g, row) + 40, kind=1, buttons=1)
        assert g.value('dnd') == 1, 'the row never became a drag'
        assert g.call('wm_dnd_target', clear[0], 300, g.scratch + 5120, 64) == 0
        assert g.string(g.scratch + 5120, 64) == 'desktop'
        assert g.call('wm_dnd_drop', clear[0], 300) == 0
        assert g.value('dnd') == 0, 'the drop left a drag running'
        assert desk_find(g, kind=1, path='home/gesture.txt') >= 0, 'the drop left no shortcut'

        # ------------------------------------------------------ drop a file onto a folder row = move
        close_others(g, w)
        assert g.call('vfs_mkdir', stringarg(g, 'home/inbox')) != 0
        g.call('vfs_write', stringarg(g, 'home/move-me.txt'), stringarg(g, 'payload', 256), 7)
        files_refresh(g, w)
        dirrow, srcrow = row_of(g, w, 'inbox'), row_of(g, w, 'move-me.txt')
        assert dirrow >= 0 and srcrow >= 0, 'the new folder or file is not listed'
        app_mouse(g, w, 120, row_y(g, srcrow))
        app_mouse(g, w, 180, row_y(g, srcrow) + 40, kind=1, buttons=1)
        assert g.value('dnd') == 1, 'the second drag never started'
        # The point is inside the window, over the folder's row: that means "move it in there", not
        # "open the folder", and only File Explorer can tell the two apart.
        dx, dy = 120 + 81, 80 + WIN_TITLEBAR + row_y(g, dirrow)
        assert g.call('wm_dnd_target', dx, dy, g.scratch + 5120, 64) == 0
        ge = struct.unpack('<4i', g.debug.read(w + 88, 16))
        fstate = files_of(g, w)
        assert g.string(g.scratch + 5120, 64) == 'app-window:files', \
            ('target %r geom %r rows %d/%d n %d notice %d wins %d'
             % (g.string(g.scratch + 5120, 64), ge, dirrow, srcrow, fstate.n, fstate.notice,
                g.call('wm_win_count')))
        assert g.call('wm_dnd_drop', dx, dy) == 0
        assert g.read('home/move-me.txt') is None, 'the file is still where it was'
        assert g.read('home/inbox/move-me.txt') == b'payload', 'the file did not arrive in the folder'

        # A file row is not a container: the drop falls through to the window manager's meaning, which
        # opens it, and the file stays where it is.
        g.call('vfs_write', stringarg(g, 'home/second.txt'), stringarg(g, 'two', 256), 3)
        files_refresh(g, w)
        secrow, tgtrow = row_of(g, w, 'second.txt'), row_of(g, w, 'gesture.txt')
        assert secrow >= 0 and tgtrow >= 0
        app_mouse(g, w, 120, row_y(g, secrow))
        app_mouse(g, w, 180, row_y(g, secrow) + 40, kind=1, buttons=1)
        assert g.call('wm_dnd_drop', 120 + 81, 80 + WIN_TITLEBAR + row_y(g, tgtrow)) == 0
        assert g.read('home/second.txt') == b'two', 'a drop on a file row moved the file anyway'
        while g.call('wm_win_count') > 1:
            g.close(g.call('wm_win_at', 1))

        # ------------------------------------------------------ the bar: a pair puts a window away
        # System Monitor is single-instance, so the first press of the pair raises the window it already
        # has instead of adding a second one, and the second press is the only thing that can change the
        # state.  Notepad would not show this: a click there legitimately opens another window.
        assert callstr(g, 'wm_taskbar_pin', 'sysmon') != 0
        g.call('wm_open_app', stringarg(g, 'sysmon'), 0)
        g.run(.6)
        assert g.call('wm_app_minimized', stringarg(g, 'sysmon')) == 0
        bx, by = bar_slot(g, 'sysmon')
        click(g, bx, by)              # first press: an ordinary click, it raises
        press(g, bx, by)              # second press inside the window: put it away
        assert g.call('wm_app_minimized', stringarg(g, 'sysmon')) == 1, \
            'double-clicking the bar entry did not put the window away'
        release(g, bx, by)
        assert g.call('wm_app_minimized', stringarg(g, 'sysmon')) == 1, 'the release undid the minimise'
        g.call('paint_all')
        g.snapshot('affordances-bar-put-away')
        click(g, bx, by)              # one click brings it back
        assert g.call('wm_app_minimized', stringarg(g, 'sysmon')) == 0, 'the entry would not bring it back'
        g.snapshot('affordances-bar-restored')
    print('affordances: ok')


if __name__ == '__main__':
    test()
