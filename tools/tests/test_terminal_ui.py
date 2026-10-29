"""What the terminal actually paints while a command's answer arrives.

The owner's two reports (2026-10-28) were both about the picture, not the data:

  * a command with long output painted "a long column of `user@scos`" before the
    answer filled in - the typewriter laid out its preview by counting NEWLINES
    while the painter placed it by counting wrapped ROWS, so every row the model
    did not know about fell through to the prompt branch and was drawn as one
    more prompt;
  * typing while the prompt had scrolled out of view left the keys invisible
    until something else happened to redraw at the bottom.

A screenshot proves that something is wrong on one frame of one machine; it does
not say which number was wrong.  The terminal therefore keeps the last frame's
arithmetic (`term_debug`), and this suite asserts on the arithmetic: while an
answer is still being typed, no row may be painted as a prompt, and any input
takes the view back to the bottom.  The pixels are the consequence, so a run
here that passes means the picture cannot show those two artefacts.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

from qemu_support import Guest          # noqa: E402

DBG_PROMPT, DBG_ROWS, DBG_PENDING, DBG_TOTAL = 0, 1, 2, 3
DBG_SCROLL, DBG_VISIBLE, DBG_FINISHED, DBG_NLINES = 4, 5, 6, 7
DBG_SHOWN, DBG_LEN = 8, 9


def send(g, text, chunk=8):
    """Type at human pace: the PS/2 queue is small and a burst typed while the
    desktop repaints is dropped by the controller, not by the terminal."""
    for i in range(0, len(text), chunk):
        g.type(text[i:i + chunk])
        g.run(.12)


def focus(g, win):
    g.call('wm_focus', win)
    g.run(.15)


def key(g, name, settle=.25):
    """A key only reaches the guest while it runs: press with the VM going."""
    g.debug.send('c')
    g.press(name)
    g.pause()
    g.run(settle)


def debug(g, w, which):
    return g.call('term_debug', w, which)


def settle(g, w, seconds=4.0):
    """Wait until the typewriter has landed (bounded, so a stuck one fails the
    test that wanted to look at the finished screen instead of hanging)."""
    for _ in range(int(seconds / 0.1)):
        if debug(g, w, DBG_PENDING) == 0:
            return True
        g.run(.1)
    return debug(g, w, DBG_PENDING) == 0


def test_a_long_answer_never_paints_a_prompt_column():
    with Guest('termtype') as g:
        term = g.launch('terminal')
        focus(g, term)
        assert debug(g, term, DBG_PROMPT) == 1, 'the idle terminal must show its prompt'
        assert debug(g, term, DBG_PENDING) == 0

        # `graphics fsp' prints a paragraph that wraps in a 700px window: the
        # exact shape that used to produce extra prompt rows.
        g.type('graphics fsp')
        g.run(.3)
        assert debug(g, term, DBG_PROMPT) == 1, 'typing a command must still show the prompt'
        key(g, 'ret', settle=.05)

        worst_prompt, saw_pending, samples = 1, False, 0
        for _ in range(40):
            prompt = debug(g, term, DBG_PROMPT)
            pending = debug(g, term, DBG_PENDING)
            samples += 1
            if pending > 0:
                saw_pending = True
                # An unfinished answer owns the rows under it.  A prompt painted
                # among them is the artefact the owner saw.
                assert prompt == 0, ('a prompt was painted inside a typing answer: '
                                     '%d prompt rows, %d pending rows' % (prompt, pending))
            assert prompt <= 1, 'the prompt was painted %d times in one frame' % prompt
            worst_prompt = max(worst_prompt, prompt)
            g.run(.05)
            if debug(g, term, DBG_PENDING) == 0 and prompt == 1:
                break

        assert samples > 1
        assert saw_pending, 'the typewriter never engaged: the artefact was not exercised'
        # ... and once it has landed there is exactly one prompt, after the answer.
        for _ in range(30):
            if debug(g, term, DBG_PENDING) == 0:
                break
            g.run(.2)
        assert debug(g, term, DBG_PENDING) == 0, 'the answer never finished typing'
        assert debug(g, term, DBG_PROMPT) == 1
        total, finished = debug(g, term, DBG_TOTAL), debug(g, term, DBG_FINISHED)
        assert total == finished + 1, ('prompt row accounting: total=%d finished=%d' % (total, finished))
        assert debug(g, term, DBG_NLINES) >= 5, 'the answer lost lines'
        assert debug(g, term, DBG_ROWS) <= debug(g, term, DBG_VISIBLE)
        print('PASS: a typing answer owns its rows and paints no prompt in them', flush=True)


def test_typing_brings_the_prompt_back_into_view():
    with Guest('termscroll') as g:
        term = g.launch('terminal')
        focus(g, term)
        # Two help pages: forty-odd wrapped rows, comfortably more than the
        # window can show, so the bottom really is out of view.
        send(g, 'help\n')
        send(g, 'help\n')
        assert settle(g, term), 'the help output never finished typing'
        total, rows = debug(g, term, DBG_TOTAL), debug(g, term, DBG_VISIBLE)
        assert total > rows + 4, ('not enough output to scroll: total=%d visible=%d' % (total, rows))
        assert debug(g, term, DBG_SCROLL) == total - rows, 'the view did not follow new output'

        # Read back: PageUp moves the view away from the prompt ...
        key(g, 'pgup', settle=.3)
        scrolled = debug(g, term, DBG_SCROLL)
        assert scrolled < total - rows, ('PageUp did not scroll back: %d of %d' % (scrolled, total - rows))

        # ... and typing puts it back, because starting a command means seeing it.
        g.type('l')
        g.run(.35)
        bottom = debug(g, term, DBG_TOTAL) - debug(g, term, DBG_VISIBLE)
        assert debug(g, term, DBG_SCROLL) >= bottom, \
            'typing left the prompt out of view: row %d of %d' % (debug(g, term, DBG_SCROLL), bottom)
        assert debug(g, term, DBG_PROMPT) == 1
        print('PASS: typing returns the view to the prompt', flush=True)


def test_a_long_command_line_stays_on_the_prompt_row():
    """The input line is one row by construction; a 200-character command must
    not push the prompt out of the window or paint a second one."""
    with Guest('termlong') as g:
        term = g.launch('terminal')
        focus(g, term)
        g.type('echo ' + 'abcdefghij' * 18)
        g.run(1.2)
        assert debug(g, term, DBG_PROMPT) == 1
        assert debug(g, term, DBG_PENDING) == 0
        bottom = debug(g, term, DBG_TOTAL) - debug(g, term, DBG_VISIBLE)
        assert debug(g, term, DBG_SCROLL) >= bottom, 'a long input line scrolled the prompt away'
        key(g, 'esc')
        g.run(.2)
        assert 'panic' not in g.serial().lower()
        print('PASS: a long input line keeps its single prompt row', flush=True)


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_') and callable(fn):
            fn()
    print('all terminal UI tests passed')
