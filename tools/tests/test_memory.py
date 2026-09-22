"""What the kernel holds for itself at idle, and whether the boot arena is sized to need.

This suite exists because the machine reported ~17 MiB of "kernel" memory while the kernel image is ~1 MiB.
The difference was the fixed boot arena: the stub reserves one contiguous block below 4 GiB, the kernel
excludes the whole of it from the page allocator, and SysMon counts the reservation as kernel memory.  A
reservation that big was not a reserve for anything - the page-table pool it exists to provide takes dozens
of pages - so the unused middle was simply RAM nobody could use, on a 128 MiB target that is a tenth of
memory, and it is exactly the wrong trade in front of a browser.

So the numbers here are the point: the reservation must be small, the free total must show the difference,
and the pool must demonstrably suffice at boot (nothing taken from the allocator) while the fallback that
makes the size non-critical is exercised by the assertion's own bound.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_support import Guest, RESULTS

ROOT = Path(__file__).resolve().parents[2]

KB = 1024          # every *_kb value below is in KiB, so a MiB bound is N * KB
MEB = 1024 * KB     # kept for the prints, which divide by KB


def test_idle_reservation_is_small():
    if not (ROOT / '.tools' / 'qemu' / 'bin' / 'qemu-system-x86_64').exists():
        raise AssertionError('QEMU bundle missing; run tools/setup_qemu.py before this suite')
    with Guest('memory') as g:
        reserved_kb = g.call('memory_reserved_pages') * 4
        free_kb = g.call('memory_free_pages') * 4
        kernel_kb = g.call('proc_kernel_mem_kb')
        total_kb = g.call('mm_total_kb')
        heap_free_kb = g.call('mm_free_kb')
        from_arena = g.call('memory_table_pages', 1)
        from_allocator = g.call('memory_table_pages', 0)
        print(f'reserved (kernel image + boot arena): {reserved_kb / KB:.1f} MiB')
        print(f'SysMon kernel figure: {kernel_kb / KB:.1f} MiB; free pages: {free_kb / KB:.1f} MiB')
        print(f'heap view: total {total_kb / KB:.1f} MiB, free {heap_free_kb / KB:.1f} MiB')
        print(f'page-table frames: {from_arena} from the arena pool, {from_allocator} from the allocator')

        # The reservation must be a few MiB, not tens: it is the kernel image plus the arena, and the
        # arena's own layout (256 KiB prefix, 1.5 MiB table pool, 256 KiB module region) is 2.25 MiB.
        assert reserved_kb < 6 * KB, f'boot reservation is {reserved_kb / KB:.1f} MiB; expected a few MiB'
        assert kernel_kb < 8 * KB, f'SysMon reports {kernel_kb / KB:.1f} MiB of kernel memory at idle'
        # The difference has to be visible where it matters: usable free RAM.  QEMU is started with
        # 128 MiB but OVMF keeps part of it (its own code, the memory map, the GOP framebuffer's
        # backing), so the figure to compare against is the measured 72 MiB free and 82 MiB total, not
        # the nominal 128: before the arena was sized to need this was 59.8 MiB free of 84 MiB total.
        assert free_kb > 68 * KB, f'only {free_kb / KB:.1f} MiB free of the ~82 MiB the firmware leaves'
        assert total_kb > 70 * KB, f'the accounting lost memory: {total_kb / KB:.1f} MiB total'
        # The pool suffices at boot, so shrinking the arena did not move a problem somewhere else; and if
        # a future change needs more table frames than the pool holds, the allocator picks it up instead
        # of panicking, which is what makes the size a convenience rather than a ceiling.
        assert 0 < from_arena < 512, f'{from_arena} table frames at boot: the pool sizing needs review'
        assert from_allocator == 0, 'boot needed the allocator fallback; the pool is too small'

        # And the boot log must say the same thing the symbols do, because the log is what a user with a
        # serial cable actually reads.
        log = g.serial()
        line = next(l for l in log.splitlines() if 'free conventional pages' in l)
        at_boot = int(re.search(r'free conventional pages: 0x([0-9a-f]+)', line).group(1), 16)
        # Free pages only ever go down after memory_init publishes them, and the boot-time count is the
        # place a 16 MiB reservation showed up first: it read 0x40e4 (16,612) before the arena was sized
        # to need, which is 3,584 pages - 14 MiB - less than a 2 MiB arena leaves.
        assert at_boot * 4 >= free_kb, (line, free_kb)
        assert at_boot > 19_000, f'{at_boot} free pages at init; the arena is holding RAM again'
        print(f'PASS idle memory held to {reserved_kb / KB:.1f} MiB, {free_kb / KB:.1f} MiB free: USB')


def test_apps_stay_within_their_share():
    """Allocation has to move the numbers, in the right direction, and come back."""
    with Guest('memory-alloc') as g:
        before_free = g.call('memory_free_pages') * 4
        before_kernel = g.call('proc_kernel_mem_kb')
        # A window's worth of heap each: opening apps is the largest single allocation the desktop
        # makes, and every one of these is a built-in that does not need a document to start.
        for app in ('terminal', 'files', 'settings', 'notepad', 'studio'):
            g.launch(app)
        g.run(0.4)
        g.call('paint_all')
        after_free = g.call('memory_free_pages') * 4
        after_kernel = g.call('proc_kernel_mem_kb')
        print(f'after 5 apps: free {before_free / KB:.1f} -> {after_free / KB:.1f} MiB, '
              f'kernel {before_kernel / KB:.1f} -> {after_kernel / KB:.1f} MiB')
        assert after_free < before_free, 'launching five apps consumed no memory: the accounting is fixed'
        assert before_free - after_free < 40 * KB, 'five apps took more than 40 MiB; something leaks'
        assert after_kernel - before_kernel < 4 * KB, \
            'launching apps grew the kernel reservation, which is not what it is for'
        assert g.call('wm_win_count') >= 3, 'the apps did not come up, so this proves nothing'
        print('PASS app allocation is accounted against real free memory: USB')


def test():
    RESULTS.mkdir(parents=True, exist_ok=True)
    test_idle_reservation_is_small()
    test_apps_stay_within_their_share()


if __name__ == '__main__':
    test()
    print('PASS: the kernel holds a few MiB at idle, the boot arena is sized to need, and app memory is '
          'accounted against real free RAM')
