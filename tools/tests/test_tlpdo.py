"""tlpdo: the password gate for privileged commands (the sudo equivalent).

What is proven here:
  * the privileged-command table (what needs a password and what does not);
  * a privileged command is refused while there is no elevation, and allowed
    after one, and refused again after `security_elevate_drop`;
  * elevation is bound to inactivity, not to a wall clock that fires while the
    owner is still working;
  * the terminal asks for the password in its own masked field, a wrong
    password grants nothing and does not run the command, and the right one
    runs it (the confirmation question comes after the password);
  * `sudo.password` is a user choice: off means no prompt.
"""
import struct
from qemu_support import Guest
from test_cat import package


def stringarg(g, text, offset=0):
    g.debug.write(g.scratch + offset, text.encode() + b'\0')
    return g.scratch + offset


def callstr(g, name, text, offset=0):
    return g.call(name, stringarg(g, text, offset))


def priv(g, line):
    """(needs_password, description) from the kernel's own table."""
    g.debug.write(g.scratch + 8192, line.encode() + b'\0')
    g.debug.write(g.scratch + 16384, b'\0' * 128)
    needs = g.call('security_privileged_command', g.scratch + 8192, g.scratch + 16384, 128)
    return needs, g.string(g.scratch + 16384, 128)


def allowed(g, line):
    g.debug.write(g.scratch + 8192, line.encode() + b'\0')
    g.debug.write(g.scratch + 16384, b'\0' * 512)
    ok = g.call('security_command_allowed', g.scratch + 8192, g.scratch + 16384, 512)
    return ok, g.string(g.scratch + 16384, 512)


def transcript(g, win, tail=40):
    """Tail of a terminal tab's own line buffer (lines are 256 bytes each, 512
    of them, and the count follows them)."""
    tw = g.ptr(win + 80)
    tab = g.ptr(tw)
    n = int.from_bytes(g.debug.read(tab + 256 + 512 * 256, 4), 'little')
    return [g.debug.read(tab + 256 + 256 * i, 256).split(b'\0')[0].decode(errors='replace')
            for i in range(max(0, n - tail), n)]


def install(g, app_id):
    g.save('home/apps/' + app_id + '.cat',
           package("assert(scos.write('kept.txt','kept'));return {}", id=app_id))
    w = g.launch('home/apps/' + app_id + '.cat')
    assert g.state(w) == (0, ''), g.state(w)
    assert callstr(g, 'wm_desktop_install', app_id)
    return w


def test():
    with Guest('tlpdo') as g:
        # ---- the table ----------------------------------------------------
        for line in ('save', 'shutdown', 'poweroff', 'reboot', 'panic', 'sysrq',
                     'appuninstall demo', 'make', 'useradd bob', 'userdel bob',
                     'usermod bob admin', 'rm -f x', 'rm -s x', 'rm -i x',
                     'kill --system 12', 'theme midnight', 'isolate demo always',
                     'choices set roles.enabled on', 'policy idle 5', 'audit clear'):
            needs, what = priv(g, line)
            assert needs == 1, '%r is privileged but the table says it is not' % line
            assert what, 'no description for %r' % line
        for line in ('help', 'ls', 'cat home/documents/welcome.txt', 'save-all',
                     'rm home/documents/welcome.txt', 'kill 12', 'theme',
                     'theme list', 'security', 'tlpdo save'):
            needs, what = priv(g, line)
            assert needs == 0, '%r was wrongly treated as privileged' % line

        # ---- gating and elevation ----------------------------------------
        ok, msg = allowed(g, 'save')
        assert ok == 0 and 'tlpdo' in msg, (ok, msg)
        assert g.call('security_elevated') == 0
        assert allowed(g, 'help')[0] == 1
        g.call('security_elevate_grant')
        assert g.call('security_elevated') == 1
        assert allowed(g, 'save')[0] == 1, 'a granted elevation did not authorise the command'
        ok, msg = allowed(g, 'rm -f system/boot.log')
        assert ok == 1, msg

        # Inactivity, not wall clock: while the owner interacts it stays, and
        # after three quiet "minutes" it is gone without any timer thread.
        g.call('security_note_activity')
        assert g.call('security_elevated') == 1
        ticks = g.value('timer_ticks')
        g.debug.write(g.base + g.symbols['input_last_tick'],
                      struct.pack('<I', (ticks - 3 * 6000) & 0xffffffff))
        assert g.call('security_elevated') == 0, 'elevation survived three idle minutes'
        assert allowed(g, 'save')[0] == 0
        g.call('security_elevate_grant')
        g.call('security_note_activity')
        assert g.call('security_elevated') == 1, 'activity did not keep the elevation alive'
        g.call('security_elevate_drop')
        assert g.call('security_elevated') == 0

        # ---- the terminal asks in its own field ---------------------------
        install(g, 'priv-test')
        term = g.launch('terminal')
        g.type('appuninstall priv-test\n')
        g.run(.3)
        assert g.read('home/apps/priv-test.cat'), 'a bare appuninstall was executed'
        assert g.call('security_elevated') == 0
        g.type('tlpdo appuninstall priv-test\n')
        g.run(.3)
        g.type('definitely-wrong\n')
        g.run(.3)
        assert g.call('security_elevated') == 0, 'a wrong password granted privilege'
        assert g.read('home/apps/priv-test.cat')
        g.type('scos\n')
        g.run(.4)
        assert g.call('security_elevated') == 1, 'the correct password did not grant privilege'
        g.type('y\n')
        g.run(.5)
        assert g.read('home/apps/priv-test.cat') is None, 'the confirmed uninstall did not run'
        assert g.read('home/appdata/root/priv-test/kept.txt') is None

        # ---- the password is masked, and three wrong tries abort ----------
        install(g, 'priv-two')
        g.call('wm_focus', term)          # the app window took the focus
        g.run(.2)
        g.call('security_elevate_drop')
        g.type('tlpdo appuninstall priv-two\n')
        g.run(.3)
        # The field owns the keyboard: `y` here is a password character, not an
        # answer to a question that has not been asked yet.
        g.type('y\n')
        g.run(.3)
        assert g.read('home/apps/priv-two.cat'), 'a y at the password field ran the command'
        for attempt in range(2):
            g.type('wrong-%d\n' % attempt)
            g.run(.3)
            assert g.read('home/apps/priv-two.cat'), 'a failed password ran the command'
        assert g.call('security_elevated') == 0, 'wrong passwords granted privilege'
        lines = transcript(g, term)
        assert any('too many attempts' in l for l in lines), lines[-6:]
        assert not any('wrong-' in l for l in lines), 'a typed password reached the transcript'

        # The shell is usable again after the abort, and Esc cancels the field.
        g.type('help\n')
        g.run(.3)
        assert any('page 1/' in l for l in transcript(g, term)), transcript(g, term)[-5:]
        g.type('tlpdo appuninstall priv-two\n')
        g.run(.3)
        g.debug.send('c'); g.press('esc'); g.pause(); g.run(.2)
        assert g.call('security_elevated') == 0, 'Esc granted privilege'
        assert g.read('home/apps/priv-two.cat')

        # Nesting is refused; an empty tlpdo prints its usage.
        g.type('tlpdo tlpdo appuninstall priv-two\n')
        g.run(.3)
        assert any('do not nest' in l for l in transcript(g, term)), transcript(g, term)[-4:]
        g.type('tlpdo\n')
        g.run(.3)
        assert any('usage: tlpdo' in l for l in transcript(g, term)), transcript(g, term)[-4:]

        # ---- a grant that expires while the y/n question waits -------------
        # The question field can stay on screen longer than the elevation
        # lasts, so the gate is re-checked when the answer arrives.
        g.type('tlpdo appuninstall priv-two\n')
        g.run(.3)
        g.type('scos\n')
        g.run(.4)
        assert g.call('security_elevated') == 1, 'the correct password did not grant privilege'
        assert g.read('home/apps/priv-two.cat')
        ticks = g.value('timer_ticks')
        g.debug.write(g.base + g.symbols['input_last_tick'],
                      struct.pack('<I', (ticks - 3 * 6000) & 0xffffffff))
        assert g.call('security_elevated') == 0, 'elevation survived three idle minutes'
        g.type('y\n')
        g.run(.5)
        assert g.read('home/apps/priv-two.cat'), \
            'a confirmation answered after the grant expired still ran the command'
        assert any('needs a password' in l for l in transcript(g, term)), transcript(g, term)[-6:]

        g.call('security_elevate_grant')     # for the status line below
        # `security` reports the gate state truthfully.
        g.debug.write(g.scratch + 8192, b'security\0')
        g.debug.write(g.scratch + 16384, b'\0' * 2048)
        g.call('security_shell', g.scratch + 8192, g.scratch + 16384, 2048)
        status = g.string(g.scratch + 16384, 2048)
        assert 'privilege' in status and 'elevated' in status, status

        # The gate is a user choice.  Off means the command runs without a
        # password (and on this RAM-only machine it honestly reports no disk).
        g.debug.write(g.scratch, b'sudo.password\0')
        assert g.call('security_choice_set', g.scratch, 0) == 1
        g.call('security_elevate_drop')
        assert allowed(g, 'save')[0] == 1
        g.debug.write(g.scratch, b'sudo.password\0')
        assert g.call('security_choice_set', g.scratch, 1) == 1
        assert allowed(g, 'save')[0] == 0
        g.close(term)
    print('PASS tlpdo table, gating, inactivity-bound elevation, masked terminal '
          'password, wrong-password refusal and the sudo.password choice')


def test_a_guest_cannot_use_tlpdo():
    """Guests stay read-only: tlpdo is not on their allow-list and does not
    create an exception."""
    with Guest('tlpdo-guest') as g:
        g.call('security_api_create_user', stringarg(g, 'viewer'), 2,
               stringarg(g, 'Km4!dV9#pLq2Xz7%', 64))
        g.debug.write(g.scratch, b'roles.enabled\0')
        assert g.call('security_choice_set', g.scratch, 1) == 1
        g.debug.write(g.scratch + 8192, b'viewer\0')
        g.debug.write(g.scratch + 16384, b'Km4!dV9#pLq2Xz7%\0')
        g.debug.write(g.scratch + 24576, b'\0' * 192)
        assert g.call('security_api_authenticate', g.scratch + 8192,
                      g.scratch + 16384, g.scratch + 24576, 192) == 1
        for line in ('tlpdo save', 'save', 'tlpdo appuninstall demo', 'rm -f x'):
            ok, msg = allowed(g, line)
            assert ok == 0, '%r was allowed for a guest: %s' % (line, msg)
        assert allowed(g, 'ls')[0] == 1
    print('PASS a guest cannot reach tlpdo or any privileged command')


if __name__ == '__main__':
    test()
    test_a_guest_cannot_use_tlpdo()
