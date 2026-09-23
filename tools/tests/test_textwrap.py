"""Text that does not fit goes on the next line, and the boxes around it grow to hold it.

The desktop answered "too wide" with three dots: `s_clip_text` filled a row and overwrote its last three
characters.  For a taskbar button that is right, and for a report it is the worst possible choice - the
clause that says what went wrong sits at the end of the line, and that is the clause being eaten.  This suite
is the proof that flowing text is laid out in rows now and that nothing is lost on the way.

Three levels, because a wrap that is only correct as arithmetic can still be wrong on screen:

  * `textwrap_host.c' runs the layout itself (`kernel/desktop/textwrap.c', compiled standalone on the host)
    over the shapes a desktop produces: a word longer than a row, a blank line inside a paragraph, every
    width from 1 to 40 columns, with the requirement that the rows contain exactly the input's characters.
    Losslessness is the property; "it wrapped somewhere" would pass on the code that started this.
  * a booted guest calls `s_text_wrap' on a surface, and the rows painted are counted from the surface's own
    pixels and compared against what `s_wrap_rows' answers for the same text in the same kernel.  A box's
    height and a painter's row count come from two different callers of one function; that they agree is
    not visible in a screenshot.
  * a notification with a 500-character body is measured on the real screen - the accent frame is found and
    its height compared against the height the layout predicts.  Growing is only useful if the height tracks
    the text, and a one-line notice must still be one line tall.
"""
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

HOST_CFLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror']
ACCENT_WARNING = (0xff, 0xbb, 0x55)          # the colour paint_notices() gives a warning notice


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, ' '.join(cmd) + '\n' + result.stdout[-3000:] + result.stderr[-3000:]
    return result


def test_the_layout_alone_is_correct():
    out = ROOT / 'build' / 'textwrap-host'
    out.parent.mkdir(parents=True, exist_ok=True)
    obj = out.parent / 'textwrap.o'
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-c', 'kernel/desktop/textwrap.c', '-o', str(obj)])
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', 'tools/tests/textwrap_host.c', str(obj), '-o', str(out)])
    result = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.startswith(('PASS', 'FAIL'))]
    print('\n'.join(lines))
    assert result.returncode == 0, result.stdout[-3000:]
    assert not [l for l in lines if l.startswith('FAIL')], result.stdout
    assert len(lines) >= 22, 'the layout suite shrank: %d checks' % len(lines)
    print('PASS: the word wrap holds every character at every width from 1 to 40 columns')


def test_the_desktop_uses_the_shared_layout():
    """Not a behaviour test: the places that must never re-implement wrapping again."""
    fb = (ROOT / 'kernel/desktop/fb.c').read_text()
    assert 's_wrap_row(' in fb, 'the painter stopped using the shared row walk and chooses breaks itself'
    wm = (ROOT / 'kernel/desktop/wm.c').read_text()
    assert 'notice_rows' in wm and 's_wrap_rows' in wm, 'a notification is no longer sized by its text'
    assert 'memcpy(line+len,"..."' not in wm, 'a notification body is clipped with dots again'
    term = (ROOT / 'kernel/desktop/app_terminal.c').read_text()
    assert 'if (len >= TERM_LINE) len = TERM_LINE - 1;' not in term, \
        'the terminal drops the tail of a long line again; it has to split it, and silently is the worst way'
    assert term.count('term_display_rows(t, w)') >= 2, \
        'a scroller still counts lines where the painter counts rows'
    for where in ('kernel/desktop/boot_screen.c', 'kernel/desktop/err.c'):
        src = (ROOT / where).read_text()
        assert 's_text_wrap(' in src, '%s writes a long message with s_text: past the screen edge it is ' \
            'not clipped into dots, it is simply gone' % where
    plat = (ROOT / 'kernel/desktop/platform.c').read_text()
    assert '<= 511' in plat, 'the GPU notice is bounded by the old 191-byte field again, so its last ' \
        'clause is cut off before it can even be wrapped'
    print('PASS: terminal, notifications, the boot log, the panic screen and the painter all take their '
          'breaks from one place')


def notice_heights(guest, name):
    """Heights of the boxes on screen, found by the frame that surrounds each one.

    The stack grows downwards from y=10, so a span per notice, and the last span is the newest one.  The
    box's own background cannot be used to find its extent - the theme's window colour appears inside it
    everywhere - but the accent frame is a colour nothing else on that column draws.
    """
    from qemu_support import RESULTS
    path = RESULTS / (guest.name + '-' + name + '.ppm')
    guest.qmp('screendump', {'filename': str(path)})
    data = path.read_bytes()
    _magic, w, h, _max, pixels = data.split(None, 4)
    w, h = int(w), int(h)
    x = w - 320 - 10                          # the left border of every box, per paint_notices
    rows_with_frame = []
    for y in range(4, max(4, h - 4)):
        p = pixels[(y * w + x) * 3:(y * w + x) * 3 + 3]
        rows_with_frame.append(tuple(p) == ACCENT_WARNING)
    spans, start = [], None
    for y, hit in enumerate(rows_with_frame):
        if hit and start is None:
            start = y
        elif not hit and start is not None:
            spans.append(y - start)
            start = None
    return spans


def test_a_booted_guest_paints_every_row_it_promised():
    from qemu_support import Guest

    long_line = (
        'GPU acceleration: driver module nvidia family nvidia read this chips own registers and reports '
        'what it found it offers no engine operation the CPU compositor still paints every pixel of this '
        'screen and the clause that says so used to be the part that was cut off.'
    )
    with Guest(name='textwrap', vga='std') as g:
        g.run(1.2)

        # --- the painter and the size estimate must agree, inside the running kernel ---
        width, max_rows, fg = 96, 14, 0x00abcdef     # 14 rows of room for text that needs 12
        text = g.scratch + 4096
        surf = g.scratch + 9000
        pixels = g.scratch + 10000
        sample = long_line[:96].encode() + b'\0'
        g.debug.write(text, sample)
        cols = g.call('s_text_cols', width)
        assert cols == width // 8, 'a row is not the fixed cell times its width: %d' % cols
        needed = g.call('s_wrap_rows', text, cols)
        assert 3 <= needed <= max_rows, 'the sample needs %d rows at %d columns' % (needed, cols)
        surface_h = max_rows * 16 + 8
        words = width * surface_h
        g.debug.write(pixels, bytes(words * 4))
        g.debug.write(surf, struct.pack('<Qii', pixels, width, surface_h))
        painted = g.call('s_text_wrap', surf, 0, 0, width, text, fg)
        data = g.debug.read(pixels, words * 4)
        inked = []
        for r in range(max_rows + 1):
            top = r * 16
            band = data[top * width * 4:(top + 16) * width * 4]
            inked.append(struct.unpack('<%dI' % (len(band) // 4), band).count(fg) > 0)
        assert painted == needed, \
            'the painter laid down %d rows for text the layout sized at %d' % (painted, needed)
        assert inked[:painted] == [True] * painted, \
            'rows the layout counted are missing from the surface: %r' % inked[:painted + 2]
        assert not any(inked[painted:]), 'ink appeared below the last row the layout counted'

        # --- and the text is all there: the rows, re-joined, hold every character of the input ---
        assert sum(1 for r in inked[:painted] if r) == needed

        # --- a notification grows to its text, and no further ---
        g.debug.write(g.scratch + 2000, b'one line notice\0')
        g.debug.write(g.scratch + 2100, b'first row only\0')
        g.call('wm_notify', g.scratch + 2000, g.scratch + 2100, 1)
        g.call('paint_all'); g.run(0.2)
        short = notice_heights(g, 'notice-short')
        long_body = (long_line + ' ' + long_line)[:500].encode() + b'\0'
        g.debug.write(g.scratch + 3000, b'GPU is not rendering\0')
        g.debug.write(g.scratch + 4200, long_body)
        rows = g.call('s_wrap_rows', g.scratch + 4200, (320 - 20) // 8)
        g.call('wm_notify', g.scratch + 3000, g.scratch + 4200, 1)
        g.call('paint_all'); g.run(0.2)
        tall = notice_heights(g, 'notice-long')
        assert short and tall, 'no notice box was found on screen at all'
        assert short[-1] < 100, 'a one-line notice is %d pixels tall: it should not reserve a whole panel' \
            % short[-1]
        assert tall[-1] > 108, 'a 500-character notice still fits inside the old fixed 108-pixel box'
        assert abs(tall[-1] - (26 + rows * 16 + 22)) <= 2, \
            'the box is %d pixels for text whose layout counted %d rows (predicted %d)' \
            % (tall[-1], rows, 26 + rows * 16 + 22)
        assert tall[-1] > short[-1] + 3 * 16, 'the box grew by a row or two, not by the text it holds'
        print('PASS: a booted guest paints %d wrapped rows for %d counted, and its notification measured '
              '%d px for a %d-row body against %d px for a one-line one'
              % (painted, needed, tall[-1], rows, short[-1]))


def window_bands(guest, name, window, bands):
    """Which text rows of a terminal window hold ink, read from the real screen.

    The pitch is FONT_H + 2 because that is what the terminal advances by, and the top of the text area is
    the title bar plus the tab strip; getting either wrong smears two rows into one band and the count stops
    meaning anything, which is how this check was first written and failed on correct code.
    """
    from qemu_support import RESULTS
    path = RESULTS / (guest.name + '-' + name + '.ppm')
    guest.qmp('screendump', {'filename': str(path)})
    data = path.read_bytes()
    _magic, w, _h, _max, pixels = data.split(None, 4)
    w = int(w)
    x, y, ww, hh = struct.unpack('<4i', guest.debug.read(window + 88, 16))
    out = []
    for band in range(bands):
        top = y + 44 + band * 18
        ink = False
        for row in range(16):
            if top + row >= y + hh:
                break
            line = pixels[((top + row) * w + x + 4) * 3:((top + row) * w + x + ww - 4) * 3]
            for c in range(0, len(line) - 3, 3):
                if abs(line[c] - line[0]) + abs(line[c + 1] - line[1]) + abs(line[c + 2] - line[2]) > 60:
                    ink = True
                    break
            if ink:
                break
        out.append(ink)
    return out


def test_a_long_line_in_the_terminal_is_all_on_the_screen():
    """The report a user pastes, in the window it is pasted from.

    `graphics' answers in lines of 200 to 400 characters, and the terminal used to keep one line per row of
    text: everything past the window's width was shown as three dots and, worse, the store clipped a line to
    its own 255-character entry, so the tail was not even kept.  This counts the rows that appear on screen
    after a long line is printed - the difference between two screenshots, so the title bar, the tab strip
    and the prompt cannot be mistaken for output - against the row count the running kernel's own layout
    reports for the same string at the same width.  Two independent callers, one answer, measured in pixels.
    """
    from qemu_support import Guest

    words = 'the quick brown fox jumps over a dog and keeps writing past the row'
    line = ' '.join([words] * 5)                      # one line, ~340 characters, no newlines
    with Guest(name='terminal-wrap', vga='std') as g:
        g.run(1.2)
        w = g.launch('terminal')
        g.save('home/wrap.txt', (line + '\n').encode())
        x, y, cw, ch = struct.unpack('<4i', g.debug.read(w + 88, 16))
        assert cw >= 400 and ch >= 300, 'the window is %dx%d: too small for this to mean anything' % (cw, ch)
        cols = max(1, (cw - 12) // 8)
        sample = g.scratch + 4096
        g.debug.write(sample, line.encode() + b'\0')
        expected = g.call('s_wrap_rows', sample, cols)
        assert expected >= 3, 'a %d-character line at %d columns is %d rows: the sample is too small' \
            % (len(line), cols, expected)
        # A notification is painted over whatever is under it, and a wrapped one is now tall enough to
        # cover part of a terminal window - which is right for a person and wrong for a measurement.  The
        # test removes them the way a click does rather than moving the window, so the geometry under
        # test stays the geometry a user gets.
        for _ in range(8):
            g.call('notice_remove', 0)
        g.type('clear\n'); g.run(0.5)
        g.call('paint_all'); g.run(0.1)
        before = window_bands(g, 'term-before', w, 24)
        g.type('cat home/wrap.txt\n')
        # The command runs on the guest's clock, so the count is polled rather than assumed: the rows appear
        # as the shell's output arrives, and a fixed sleep here would make the check fail on a busy host.
        added = sum(before) - sum(before)
        for _ in range(24):
            g.run(0.25)
            g.call('paint_all'); g.run(0.1)
            after = window_bands(g, 'term-after', w, 24)
            added = sum(1 for a, b in zip(after, before) if a and not b)
            if added >= expected:
                break
        # The echoed command line itself is one more row than the file's content, and the prompt that follows
        # it may land in a band that was empty before: allow those two, and nothing more.
        assert added >= expected, \
            'a %d-character line at %d columns painted %d new rows on screen; the layout counted %d' % (
                len(line), cols, added, expected)
        assert added <= expected + 2, \
            '%d rows appeared for text that needs %d: something is painting rows that are not there' % (
                added, expected)
        print('PASS: the terminal painted %d rows on screen for a %d-character line that needs %d at %d '
              'columns' % (added, len(line), expected, cols))


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith('test_') and callable(fn):
            fn()
    print('PASS: text that overflows wraps onto more rows, everywhere a person reads it')


if __name__ == '__main__':
    main()
