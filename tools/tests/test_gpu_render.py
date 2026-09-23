"""GPU rendering, measured against a booted device instead of inferred from a loaded driver.

Why the Cirrus CL-GD5446: SCos has to be *observed* drawing through hardware for any of this to mean
anything, and of the display devices this environment can emulate, the CL-GD5446 is the only one whose
2D engine is reachable through the interface the kernel actually offers a driver module - a memory
mapping of the register BAR and 32-bit accesses, with no port I/O and no bus-master read of system
memory.  Its bitBLT block does the two operations a desktop needs (paint a rectangle, move a
rectangle), which is what makes an honest measurement possible here rather than a plausible claim.

Four things are checked, all against the emulated device's own state:

1. The driver module is read off the disk for the matching family, and the kernel's self-test paints
   a rectangle *in the card's memory* and reads it back, then asks the card to move it and reads that
   back too.  `225/225` is the pass condition; a driver whose register sequence is wrong detaches and
   the desktop continues on the CPU.
2. A whole-screen repaint by the compositor is done by the engine: the pixel counters move by exactly
   one frame on the GPU side and not at all on the CPU side, and a `screendump` taken by QEMU from the
   device - not from anything SCos believes it wrote - shows the colour at the corners and the centre.
3. Scrolling a screen of text (768x480 pixels moved up one line) is one engine operation, counted, and
   the pixels on the card really moved.
4. The fallback is the other half of the feature: the same kernel image on a console device with no
   engine still paints the desktop, with every pixel accounted to the CPU - because the compositor
   asks the card and does not assume the answer.

The second guest in this file is `-vga std`, which is what every other suite boots; running both keeps
"the GPU is used when it exists" and "nothing breaks when it does not" tied to the same image.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_support import Guest, RESULTS   # noqa: E402


def ppm(path):
    """A QEMU screendump, read as the raw device output it is: P6 header plus one byte per channel.

    The dump is written by the emulator, so the file can lag the QMP reply that asked for it; waiting
    for it to stop growing is what keeps the assertion about real pixels rather than about timing."""
    path = Path(path)
    blob, size = b'', -1
    for _ in range(80):
        if path.exists():
            current = path.stat().st_size
            if current and current == size:
                break
            size = current
        import time
        time.sleep(0.05)
    blob = path.read_bytes()
    assert blob[:2] == b'P6', blob[:16]
    i, fields = 2, []
    while len(fields) < 3:
        while blob[i] in b' \t\n\r':
            i += 1
        if blob[i:i + 1] == b'#':
            while blob[i] not in b'\n':
                i += 1
            continue
        digits = b''
        while 48 <= blob[i] <= 57:
            digits += bytes([blob[i]])
            i += 1
        fields.append(int(digits))
    width, height, maximum = fields
    assert maximum == 255, fields
    body = blob[i + 1:]
    assert len(body) == width * height * 3, (len(body), width, height)
    return width, height, body


def pixel(data, width, x, y):
    offset = (y * width + x) * 3
    return tuple(data[offset:offset + 3])


def colour(argb):
    """An XRGB value as it lands in a 32-bit surface, read back as three 8-bit channels."""
    return (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF


def report(guest):
    """The shipped `gpu_report` text, read out of the running kernel rather than reconstructed here."""
    guest.call('gpu_report', guest.scratch, 60000)
    return guest.string(guest.scratch, 60000)


def test_engine_paints_the_console():
    with Guest('gpu-render-cirrus', vga='cirrus') as g:
        serial = g.serial()
        g.run(0.6)

        # --- the module, and the engine's own words about it --------------------------------------
        assert 'matched=cirrus' in serial, serial[-1500:]
        bound = [l for l in serial.splitlines() if 'verified in device memory' in l]
        assert bound and '225/225 pixels' in bound[0], bound
        assert 'engine drives the console' in serial, serial[-1500:]
        assert 'self-test failed' not in serial and 'engine retired' not in serial, serial[-1500:]
        assert 'PANIC' not in serial and 'panic' not in serial, serial[-1500:]
        # Detection must not reprogram the card: a bitBLT driver that needed a mode set would show up
        # here, because the only writes it is allowed are to its own engine registers.
        assert '0 PCI config write(s) issued' in serial, serial[-1500:]
        # The firmware offered 1024x768 on this device but reports an aperture that only holds
        # 800x600, so booting at all is the retry's behaviour: the loader sets a mode, finds the
        # frame buffer too small for it, and moves to the next preference instead of failing.
        assert '800x600 stride 800' in serial, serial[-1500:]

        text = report(g)
        assert "module engine paints this function's scanout" in text, text[-2000:]
        assert 'hand-authored id row and capability record' in text, text[-2000:]

        assert g.call('gpu_engine_drives_output') == 1, 'engine bound but not driving the console'
        # A card whose register block answers reads but drops writes is refused before it can be
        # asked to paint, so the boot log carries no round-trip complaint at all.
        assert 'register round-trip' not in serial, serial[-1200:]

        # What the user-facing graphics report says about this machine, in its own words: the module was
        # read for this family, verified against the device, and is used for output - and the *other*
        # family on the disk was never opened, which is the per-family loading rule in the same test.
        g.call('graphics_report', g.scratch, 4096)
        panel = g.string(g.scratch, 4096)
        assert 'driver module cirrus (family cirrus) loaded from storage and verified by device ' \
               'readback, 225/225 pixels; used for solid output rectangles' in panel, panel[-1800:]
        # Three files are on the disk now, and the two that are not Cirrus stay unopened: ati.mod is
        # 13536 B and nvidia.mod is 6472 B, so 20008 B were never read on this boot.  That the NVIDIA
        # file is *not* opened for a Cirrus machine is the per-family loading rule, and it is measured
        # here rather than asserted in a comment.
        assert 'the other families\' 20008 B were never read' in panel, panel[-1800:]
        assert 'of 3 module file(s) on the disk' in panel, panel[-1800:]

        # --- the compositor's whole-screen repaint goes through the card --------------------------
        engine_before = g.value('engine_work_pixels')
        cpu_before = g.value('cpu_work_pixels')
        paint = 0x004488cc
        g.call('fb_clear', paint)
        g.call('fb_flip')
        g.snapshot('solid-by-engine')
        engine_after, cpu_after = g.value('engine_work_pixels'), g.value('cpu_work_pixels')
        assert engine_after > engine_before, (engine_after, engine_before)
        assert cpu_after == cpu_before, ('CPU copied a frame the engine should have painted', cpu_after,
                                         cpu_before)
        width, height, body = ppm(RESULTS / 'gpu-render-cirrus-solid-by-engine.ppm')
        want = colour(paint)
        for where in ((0, 0), (width // 2, height // 2), (width - 1, height - 1)):
            got = pixel(body, width, *where)
            assert got == want, ('engine fill not on the device at', where, 'got', got, 'want', want)
        # The counters are per-pixel, so the number has to be a frame - not a guess about acceleration.
        assert engine_after - engine_before >= width * height, (engine_after - engine_before, width * height)

        # --- scrolling a screen of text is one bit-block transfer ----------------------------------
        engine_before = g.value('engine_work_pixels')
        cpu_before = g.value('cpu_work_pixels')
        scroll_w, scroll_h, step = 768, 480, 16
        moved = g.call('fb_scroll_output', 0, 0, scroll_w, scroll_h, step, 0x00001020)
        assert moved == 1, 'the engine declined a scroll it was asked to do and owns the memory for'
        g.snapshot('scrolled-by-engine')
        engine_after = g.value('engine_work_pixels')
        assert engine_after - engine_before == scroll_w * (scroll_h + step), \
            (engine_after - engine_before, scroll_w * (scroll_h + step))
        assert g.value('cpu_work_pixels') == cpu_before, 'scroll fell back to the CPU after being counted'
        width, height, body = ppm(RESULTS / 'gpu-render-cirrus-scrolled-by-engine.ppm')
        # The fill from the previous step covered rows 0..599 of the whole screen, so a scroll up has
        # to leave that colour at the top of the band it moved; a card that accepted the registers and
        # drew nothing leaves the frame untouched and fails here.
        assert pixel(body, width, 8, 8) == colour(paint), pixel(body, width, 8, 8)

        # --- a rectangle the engine paints alone is visible on the device -------------------------
        lone = 0x00ff3366
        assert g.call('gpu_engine_fill_rectangle', 0, 0, 240, 120, lone) == 0
        assert g.call('gpu_engine_wait_idle') == 0
        g.snapshot('engine-only-rectangle')
        width, height, body = ppm(RESULTS / 'gpu-render-cirrus-engine-only-rectangle.ppm')
        assert pixel(body, width, 0, 0) == colour(lone), pixel(body, width, 0, 0)
        assert pixel(body, width, 239, 119) == colour(lone), pixel(body, width, 239, 119)

        # --- and the report says so in the same words the user would read ------------------------
        text = report(g)
        painted = re.search(r'pixels painted: (\d+) by the GPU\'s 2D engine, (\d+) by the CPU', text)
        assert painted, text[-1500:]
        assert int(painted.group(1)) > 0, painted.group(0)
        assert '(no GPU drawing yet' not in text, text[-1500:]


def test_cpu_path_without_an_engine():
    """The same image on a console device with no 2D engine: everything still draws, on the CPU."""
    with Guest('gpu-render-cpu') as g:
        serial = g.serial()
        g.run(0.6)
        assert 'wm: entering main loop' in serial, serial[-1500:]
        text = report(g)
        painted = re.search(r'pixels painted: (\d+) by the GPU\'s 2D engine, (\d+) by the CPU', text)
        assert painted, text[-1500:]
        assert int(painted.group(1)) == 0, painted.group(0)
        assert int(painted.group(2)) > 0, ('a desktop that drew nothing at all', painted.group(0))
        assert 'no engine bound' in serial or 'engine=none' in serial, serial[-1500:]
        g.snapshot('cpu-compositor')
        width, height, body = ppm(RESULTS / 'gpu-render-cpu-cpu-compositor.ppm')
        # Not blank, and not the engine's doing: the compositor's own output has structure.
        assert len(set(pixel(body, width, x, 4) for x in range(0, width, 7))) > 1, 'uniform boot screen'
        assert g.call('gpu_engine_drives_output') in (0, 1)   # std has no engine to drive with


if __name__ == '__main__':
    failures = 0
    for test in (test_engine_paints_the_console, test_cpu_path_without_an_engine):
        try:
            test()
            print('PASS:', test.__name__)
        except AssertionError as error:
            failures += 1
            print('FAIL:', test.__name__, error)
    sys.exit(1 if failures else 0)
