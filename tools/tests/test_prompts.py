"""Interactive prompts: a question is answered in its own field.

The behaviour the owner asked for (2026-10-27): a y/n question and the
first-launch app choice must work like Linux - press the key in a dedicated
field, press Enter to accept - instead of typing a whole line at the shell
prompt and hoping the shell reads it as an answer.

Proven here:
  * `y` alone does not answer a confirmation, Enter accepts it;
  * Enter with nothing chosen cancels (the safe default);
  * Esc cancels without running anything;
  * `appstrt` of an installed app asks the isolation question in that field,
    pressing a letter and Enter launches with the choice, and Esc launches
    nothing;
  * the persisted choices from that field are real (always isolated).
"""
from qemu_support import Guest
from test_cat import package


def stringarg(g, text, offset=0):
    g.debug.write(g.scratch + offset, text.encode() + b'\0')
    return g.scratch + offset


def callstr(g, name, text, offset=0):
    return g.call(name, stringarg(g, text, offset))


def install(g, app_id, source="assert(scos.write('kept.txt','kept'));return {}"):
    g.save('home/apps/' + app_id + '.cat', package(source, id=app_id))
    assert g.call('lua_app_install', stringarg(g, 'home/apps/' + app_id + '.cat')), app_id
    assert callstr(g, 'wm_desktop_install', app_id)
    g.run(.5)


def close_extra(g, keep):
    while g.call('wm_win_count') > 1:
        w = g.call('wm_win_at', 1)
        if w == keep:
            w = g.call('wm_win_at', 0)
        g.close(w)


def focus(g, win):
    g.call('wm_focus', win)
    g.run(.1)


def esc_key(g):
    """Escape only reaches the guest while it is running: `press` alone would
    deliver a key to a stopped VM.  Same pattern the dialog tests use."""
    g.debug.send('c')
    g.press('esc')
    g.pause()
    g.run(.2)


def send(g, text, chunk=8):
    """Type like a person, not like a fire hose: the PS/2 queue is small and a
    burst typed while the desktop is repainting can be dropped.  Real users
    type slower than the emulator's event queue, so this is test pacing, not a
    product limitation."""
    for i in range(0, len(text), chunk):
        g.type(text[i:i + chunk])
        g.run(.12)


def test():
    with Guest('prompts') as g:
        # ---- Enter, not a bare key, accepts a confirmation -----------------
        install(g, 'confirm-app')
        term = g.launch('terminal')
        focus(g, term)
        send(g, 'tlpdo appuninstall confirm-app\n')
        g.run(.3)
        send(g, 'scos\n')
        g.run(.4)
        assert g.call('security_elevated') == 1
        send(g, 'y')                     # keyed, but not accepted yet
        g.run(.3)
        assert g.read('home/apps/confirm-app.cat'), \
            'a bare y answered the confirmation without Enter'
        send(g, '\n')
        g.run(.4)
        assert g.read('home/apps/confirm-app.cat') is None, \
            'Enter did not accept the confirmation'

        # ---- Enter with nothing chosen cancels -----------------------------
        install(g, 'cancel-app')
        send(g, 'tlpdo appuninstall cancel-app\n')
        g.run(.3)
        send(g, '\n')
        g.run(.4)
        assert g.read('home/apps/cancel-app.cat'), 'Enter confirmed a question with no answer'

        # ---- Esc cancels without running the command ----------------------
        send(g, 'tlpdo appuninstall cancel-app\n')
        g.run(.3)
        send(g, 'y')
        esc_key(g)
        assert g.read('home/apps/cancel-app.cat'), 'Esc did not cancel the confirmation'
        assert g.call('wm_win_count') == 1

        # ---- the field edits like a field, and owns the keyboard ----------
        install(g, 'widget-app')
        focus(g, term)
        send(g, 'tlpdo appuninstall widget-app\n')
        g.run(.3)
        send(g, 'scos\n')
        g.run(.4)
        assert g.call('security_elevated') == 1
        tw = g.ptr(term + 80)
        tabs_before = int.from_bytes(g.debug.read(tw + 48, 4), 'little')
        send(g, 'y')
        g.run(.2)
        g.debug.send('c'); g.press('backspace'); g.pause(); g.run(.2)
        send(g, '\n')
        g.run(.4)
        assert g.read('home/apps/widget-app.cat'), 'Backspace did not clear the answer'

        # The field owns the keyboard: a terminal shortcut must not act while a
        # question waits, and the answer is still keyed and Enter-accepted.
        send(g, 'tlpdo appuninstall widget-app\n')     # still elevated: asks at once
        g.run(.3)
        g.debug.send('c'); g.press('ctrl', 't'); g.pause(); g.run(.2)
        tabs_after = int.from_bytes(g.debug.read(tw + 48, 4), 'little')
        assert tabs_after == tabs_before, 'Ctrl+T opened a tab from inside the question'
        g.debug.send('c'); g.press('shift', 'y'); g.pause(); g.run(.2)
        send(g, '\n')
        g.run(.5)
        assert g.read('home/apps/widget-app.cat') is None, 'capital Y plus Enter did not accept'

        # ---- the first-launch app choice is a keyed field -----------------
        assert g.call('security_app_isolation_count') == 0
        g.debug.write(g.scratch, b'isolation.prompt\0')
        assert g.call('security_choice_set', g.scratch, 1) == 1
        install(g, 'launch-once')
        windows = g.call('wm_win_count')
        send(g, 'appstrt launch-once\n')
        g.run(.4)
        assert g.call('wm_win_count') == windows, 'appstrt launched before the question was answered'
        assert not callstr(g, 'wm_app_running', 'launch-once')
        send(g, 'i')                     # isolated once, still waiting for Enter
        g.run(.3)
        assert g.call('wm_win_count') == windows, 'a bare key launched the app'
        send(g, '\n')
        g.run(.5)
        assert g.call('wm_win_count') == windows + 1, 'Enter did not launch the app'
        assert g.call('proc_isolation_count') == 1, 'the isolated-once choice was ignored'
        assert callstr(g, 'wm_app_running', 'launch-once')
        close_extra(g, term)
        assert g.call('proc_isolation_count') == 0

        # Enter with nothing chosen is the documented default: unisolated once.
        install(g, 'launch-default')
        focus(g, term)
        send(g, 'appstrt launch-default\n')
        g.run(.4)
        send(g, '\n')
        g.run(.5)
        assert g.call('wm_win_count') == windows + 1, 'Enter did not launch the default choice'
        assert g.call('proc_isolation_count') == 0, 'the default launched isolated'
        close_extra(g, term)

        # Esc launches nothing and stores nothing.
        install(g, 'launch-esc')
        focus(g, term)
        send(g, 'appstrt launch-esc\n')
        g.run(.4)
        esc_key(g)
        assert g.call('wm_win_count') == windows, 'Esc still launched the app'
        assert not callstr(g, 'wm_app_running', 'launch-esc')

        # A letter plus Enter stores a real, persisted choice.
        focus(g, term)
        send(g, 'appstrt launch-esc\n')
        g.run(.4)
        send(g, 'a\n')
        g.run(.5)
        assert callstr(g, 'security_app_isolation', 'launch-esc') == 1, \
            'always-isolated was not stored'
        close_extra(g, term)
        # And it is not asked again: the next appstrt opens immediately.
        focus(g, term)
        windows = g.call('wm_win_count')
        send(g, 'appstrt launch-esc\n')
        g.run(.5)
        assert g.call('wm_win_count') == windows + 1, 'a stored choice asked again'
        assert g.call('proc_isolation_count') == 1, 'the stored choice was not applied'
        g.close(term)
    print('PASS keyed y/n with Enter-to-accept, safe-default Enter, Esc cancel, '
          'keyed first-launch isolation choice, and stored choices')


if __name__ == '__main__':
    test()
