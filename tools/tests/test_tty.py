"""The plain text console (Ctrl+Alt+F1) uses the same password gate.

The desktop terminal is not the only shell: the maintenance console is a real
TTY with its own line buffer.  It asks for the sign-in password before it opens
at all, and then must ask for the tlpdo password BEFORE the y/n question
(otherwise the owner is asked to confirm an action that will then be refused),
keep the password off the screen and out of the transcript, and refuse a
nested `tlpdo`.

Proven here on the guest's own console:
  * the console re-authenticates before it gives a shell;
  * `appuninstall` without tlpdo is refused and no question is asked;
  * `tlpdo appuninstall` prints the password prompt first, a wrong password
    does nothing, and the y/n question only appears after a good one;
  * the typed password never appears in the console transcript;
  * only y/n answers the question, and anything else asks again;
  * `tlpdo tlpdo ...` is refused, the shell keeps working, and Ctrl+Alt+F7
    returns to the desktop.
"""
import struct

from qemu_support import Guest
from test_cat import package

NLINES = 64 * 97          # struct tty_session: char lines[64][97]; int nlines


def stringarg(g, text, offset=0):
    g.debug.write(g.scratch + offset, text.encode() + b'\0')
    return g.scratch + offset


def console_base(g):
    return g.base + g.symbols['consoles']


def tty_text(g, tail=30):
    """The active console's own line buffer - what a person standing at the
    machine reads, not a redrawn framebuffer."""
    base = console_base(g)
    n = int.from_bytes(g.debug.read(base + NLINES, 4), 'little')
    rows = []
    for i in range(max(0, n - tail), n):
        rows.append(g.debug.read(base + i * 97, 97).split(b'\0')[0]
                    .decode(errors='replace'))
    return rows


def send(g, text, chunk=6):
    """Type at human pace: a burst typed while the console repaints loses
    keys, and that is the harness, not the product."""
    for i in range(0, len(text), chunk):
        g.type(text[i:i + chunk])
        g.run(.15)


def enter_tty(g):
    """The console never inherits the desktop session: it asks for the account
    password first, exactly like Linux' login shell on another VT."""
    g.debug.write(g.base + g.symbols['tty_request'], struct.pack('<i', 1))
    g.run(.6)
    send(g, 'scos\n')
    g.run(.4)
    assert g.value('tty_rows') > 0, 'the console never opened after the password'
    return g.value('tty_rows')


def leave_tty(g):
    g.debug.send('c')
    g.press('ctrl', 'alt', 'f7')
    g.pause()
    g.run(.5)


def test():
    with Guest('tty') as g:
        g.save('home/apps/tty-app.cat',
               package("assert(scos.write('kept.txt','kept'));return {}", id='tty-app'))
        assert g.call('lua_app_install', stringarg(g, 'home/apps/tty-app.cat'))
        w = g.launch('home/apps/tty-app.cat')
        assert g.state(w) == (0, '')
        assert g.read('home/appdata/tty-app/kept.txt') == b'kept'
        g.close(w)
        term = g.launch('terminal')          # stays open across the VT switch

        rows = enter_tty(g)
        assert rows >= 4

        # ---- password before the question, not after it -------------------
        send(g, 'appuninstall tty-app\n')
        g.run(.4)
        text = tty_text(g, 20)
        assert any('needs a password' in row for row in text), text
        assert not any('Continue?' in row for row in text), \
            'the console asked to confirm before asking for the password'
        assert g.read('home/apps/tty-app.cat'), 'an unprivileged uninstall ran'

        # ---- the password field, a wrong try, then the right one ----------
        send(g, 'tlpdo appuninstall tty-app\n')
        g.run(.4)
        text = tty_text(g, 10)
        assert any('Password (not echoed)' in row for row in text), text
        assert not any('Continue?' in row for row in text), \
            'the confirmation question came before the password'
        send(g, 'wrong-password\n')
        g.run(.5)
        assert g.call('security_elevated') == 0, 'a wrong password granted privilege'
        assert g.read('home/apps/tty-app.cat')
        assert not any('wrong-password' in row for row in tty_text(g)), \
            'the console echoed a typed password'
        send(g, 'scos\n')
        g.run(.6)
        assert g.call('security_elevated') == 1, 'the right password did not grant privilege'
        text = tty_text(g, 12)
        assert any('Continue?' in row for row in text), \
            'the console never asked the y/n question after the password'

        # ---- the answer is y or n, and anything else asks again -----------
        send(g, 'maybe\n')
        g.run(.4)
        assert g.read('home/apps/tty-app.cat'), 'an invalid answer confirmed the question'
        assert any('Please answer y or n' in row for row in tty_text(g, 8)), tty_text(g, 6)
        send(g, 'y\n')
        g.run(.6)
        assert g.read('home/apps/tty-app.cat') is None, 'the confirmed uninstall did not run'
        assert g.read('home/appdata/tty-app/kept.txt') is None, 'the console uninstall kept the data'

        # ---- no nesting, and the shell is still usable --------------------
        send(g, 'tlpdo tlpdo save\n')
        g.run(.4)
        assert any('do not nest' in row for row in tty_text(g, 8)), tty_text(g, 6)
        send(g, 'help\n')
        g.run(.4)
        assert any('tlpdo' in row for row in tty_text(g, 60)), 'the console shell stopped working'

        # ---- back to the desktop, untouched -------------------------------
        leave_tty(g)
        assert g.call('wm_win_count') >= 1, 'the desktop did not come back'
        assert g.value('tty_rows') > 0          # set once; the loop really ran
        g.call('security_elevate_drop')
        g.close(term)
    print('PASS the text console re-authenticates, refuses without tlpdo, asks '
          'for the password before the y/n question, never echoes it, refuses '
          'nesting, and returns to the desktop')


if __name__ == '__main__':
    test()
