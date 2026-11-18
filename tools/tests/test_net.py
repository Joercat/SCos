"""The network stack, compiled exactly as the kernel compiles it and run against a model of a wire.

`tools/tests/net_host.c` is the substance: it includes kernel/drivers/net.c verbatim and answers its frames by
hand, from the RFC field offsets, so a change in the stack's idea of a header fails here instead of on somebody's
network.  What this file adds is (a) the pinned count, so a scenario cannot quietly disappear, (b) the facts the
stack has to stay compatible with the rest of the kernel about - the adapter contract drivers implement, the
timer it runs on, the absence of a format-string function in this kernel - and (c) the delivery tests: the image
carries the driver objects, and a machine with no supported card says so without touching anything.
"""

import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

NET_CHECKS = 79                 # the model of the wire, pinned: a dropped scenario is a silent loosening
DRIVER_CHECKS = {'rtl8139': 65, 'rtl8168': 58}   # the model of the two cards, pinned for the same reason

GCC = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
       '-fno-stack-protector', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
       '-Ikernel/include']


def run(cmd, **kwargs):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kwargs)
    assert result.returncode == 0, ' '.join(str(x) for x in cmd) + '\n' + result.stdout + result.stderr
    assert not result.stderr.strip(), ' '.join(str(x) for x in cmd) + '\n' + result.stderr
    return result


def source(name):
    return (ROOT / name).read_text()


def test_the_stack_speaks_the_wire_and_reports_its_own_count():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'net_host'
        run(GCC + ['tools/tests/net_host.c', '-o', str(binary)])
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        assert not result.stderr.strip(), 'the sanitizers said something: ' + result.stderr
        m = re.match(r'^(\d+) check\(s\), (\d+) failure\(s\)$', result.stdout.strip())
        assert m, 'the network harness did not report its count: ' + result.stdout[-400:]
        assert int(m.group(1)) == NET_CHECKS, \
            'the network harness reports %s checks, pinned at %d - a scenario was dropped or added without ' \
            'this file noticing' % (m.group(1), NET_CHECKS)
        assert int(m.group(2)) == 0, result.stdout


def test_both_realtek_drivers_are_driven_against_a_model_of_their_card():
    """The drivers cannot be run on a card from here, and "it compiled" is worth nothing for code whose whole
    job is a sequence of register stores.  The harness includes each driver as the kernel does, answers PCI and
    the register file with a model, and checks what the card was told: the ring addresses, the ownership bits,
    the doorbell, the padding, the errors that must never reach the stack.  The counts are pinned so a scenario
    cannot disappear."""
    for name, expected in DRIVER_CHECKS.items():
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / name
            run(GCC + ['-DSCOS_TEST_' + name.upper(), 'tools/tests/net_drivers_host.c', '-o', str(binary)])
            result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
            assert not result.stderr.strip(), 'the sanitizers said something: ' + result.stderr
            m = re.match(r'^(\d+) check\(s\), (\d+) failure\(s\)$', result.stdout.strip())
            assert m, 'the %s harness did not report its count: %s' % (name, result.stdout[-400:])
            assert int(m.group(1)) == expected, \
                'the %s harness reports %s checks, pinned at %d' % (name, m.group(1), expected)
            assert int(m.group(2)) == 0, result.stdout


def test_the_header_is_usable_by_a_driver_and_a_host_harness_alike():
    header = source('kernel/include/net.h')
    assert not re.search(r'#\s*include\s*[<"]scos\.h[>"]', header), \
        'the adapter contract pulls the desktop in, so the host harness can no longer compile the stack'
    for include in ('<stddef.h>', '<stdint.h>'):
        assert include in header, 'net.h no longer includes %s, so its own use of size_t/uint32_t is borrowed' % include
    # Every function a driver and the panel call has to be declared here; the kernel has no implicit declarations.
    for proto in ('int  net_register_nic(struct net_nic *nic);',
                  'void net_hardware_init(void);', 'void net_start(void);', 'void net_poll(void);',
                  'int  net_set_static(', 'size_t net_format_status(', 'int net_http_get(',
                  'net_tcp_connect(', 'void net_rx_frame(', 'void net_timers(void);'):
        assert proto in header, 'the header no longer declares %r' % proto
    stack = source('kernel/drivers/net.c')
    # The stack may not need a format-string function, because the kernel has none: strings are built with the
    # helpers below, which is also why every report is bounded by its capacity.
    for banned in ('snprintf(', 'printf(', 'sprintf(', 'vsnprintf('):
        assert banned not in stack, 'the stack calls %s, which this kernel does not provide' % banned
    assert 'fmt_u32' in stack or 'put_num' in stack, 'the stack stopped formatting numbers through a helper'
    # Frames this stack will build are bounded by NET_FRAME_MAX, and the receive path has to enforce that rather
    # than accept a length a driver got wrong.
    assert re.search(r'#define NET_FRAME_MAX\s+\(NET_MTU \+ 32\)', header), \
        'the maximum frame changed shape; the MTU is the reason that number exists'


def test_the_contract_the_realtek_drivers_implement_is_the_one_the_stack_calls():
    """A driver hands over three things: a poll that drains its ring, a transmit that puts one frame on the wire,
    and the adapter's own address.  If the struct moves, both drivers have to move with it, and this is the check
    that keeps them from compiling into a stack that never calls them."""
    header = source('kernel/include/net.h')
    struct = re.search(r'struct net_nic \{(.*?)\n\};', header, re.S)
    assert struct, 'struct net_nic is not in the header any more'
    body = struct.group(1)
    for field in ('void *priv;', 'int  (*poll)(struct net_nic *);',
                  'int  (*transmit)(struct net_nic *, const void *frame, size_t len);',
                  'uint8_t mac[NET_ALEN];', 'int  link;', 'uint32_t speed_mbps;', 'const char *name;'):
        assert field in body, 'the adapter contract lost %r' % field
    stack = source('kernel/drivers/net.c')
    assert 'iface->poll(' in stack and 'iface->transmit(' in stack, \
        'the stack no longer drives an adapter through the contract drivers implement'
    assert 'NET_RX_BURST' in stack, \
        'the receive burst limit is gone, so a flooded card can keep the compositor out of its own loop'
    # One address, one route: the first adapter registered is the one used.
    assert 'net_nic_count' in stack and 'counters' in stack


def test_the_stack_runs_on_the_timer_the_rest_of_the_os_runs_on():
    stack = source('kernel/drivers/net.c')
    assert 'timer_ticks' in stack, 'the stack no longer takes time from the kernel tick'
    assert 'net_now_ms' in stack and re.search(r'net_now_ms\(void\)', stack), \
        'net_now_ms is gone, which is how every timeout in the stack is expressed'
    assert 'net_timers(' in stack, 'there is no timer entry point left for net_poll() to call'


def test_the_network_window_and_the_tray_icon_are_wired_to_this_stack():
    """The panel is where a person finds out whether they have a network.  The window reads the same
    net_get_info() the terminal does, and the status line is the same text, so the two cannot disagree."""
    panel = source('kernel/desktop/wm.c')
    assert 'paint_taskbar(' in panel, 'the taskbar is painted somewhere else now'
    taskbar = panel[panel.index('static void paint_taskbar(void)\n{'):]
    taskbar = taskbar[:taskbar.index('\n}\n')]
    assert 'ICON_NET' in taskbar or 'ICON_WIFI' in taskbar, \
        'the taskbar paints no network state, so a card that is down looks the same as a machine without one'
    header = source('kernel/include/scos.h')
    assert re.search(r'ICON_(NET|WIFI)\b', header), 'there is no network glyph in the icon enum'
    app = source('kernel/desktop/app_net.c')
    for needle in ('net_get_info(', 'net_state_name(', 'net_ping(', 'net_dhcp_renew(', 'net_http_get('):
        assert needle in app, 'the Network window never calls %s, so it cannot show or move anything' % needle
    assert 'SCOS_APP(app_net' in app, 'the Network window is not registered as an app'
    # The terminal is where the same stack is scriptable, and its status paragraph comes from the stack's own
    # writer rather than being assembled a second time in a display component.
    terminal = source('kernel/desktop/app_terminal.c')
    for needle in ('net_format_status(', 'net_parse_ip(', 'net_dns_lookup(', 'net_http_get('):
        assert needle in terminal, 'the terminal has no %s, so the network is only reachable from a window' % needle
    assert 'no TCP/IP stack' not in terminal, 'the terminal still tells the user this kernel has no network'
    platform = source('kernel/desktop/platform.c')
    order = [platform.index('usb_init();'), platform.index('net_hardware_init();'), platform.index('net_start();')]
    assert order == sorted(order), 'the network comes up before the PCI/USB machinery it shares the bus with'
    wm = source('kernel/desktop/wm.c')
    assert 'net_poll();' in wm, 'the main loop never drains the card, so no frame is ever received'
    assert wm.index('net_poll();') > wm.index('usb_poll();'), \
        'the card is polled before USB, which is not the order the loop was written for'


def test_a_machine_with_no_supported_card_still_boots_and_says_so():
    stack = source('kernel/drivers/net.c')
    absent = stack[stack.index('static void net_absent'):] if 'static void net_absent' in stack else stack
    assert 'NET_STATE_ABSENT' in stack, 'there is no state for a machine with no card at all'
    assert 'net_register_nic' in stack, 'no driver can hand the stack an adapter'
    rtl = source('kernel/drivers/rtl8139.c')
    for needle in ('0x10ec', 'pci_find_class(2, 0, 0', 'device_map(', 'net_register_nic('):
        assert needle in rtl, 'the RTL8139 driver no longer does %r' % needle
    assert 'pci_write16(' in rtl, 'the driver never enables bus mastering on its own function'
    # The driver may not map anything write-combined: a descriptor ring in a combination buffer is a ring the
    # card reads stale.
    for call in re.finditer(r'device_map\([^;]*?\)', rtl, re.S):
        assert ', 0, &' in call.group(0) or 'write_combine' not in call.group(0), \
            'a ring window asked for write combining: %s' % call.group(0).replace('\n', ' ')
    rtl8 = source('kernel/drivers/rtl8168.c')
    for needle in ('0x10ec', 'device_map(', 'net_register_nic(', 'DESC_OWN', 'DESC_FIRST_FRAG'):
        assert needle in rtl8, 'the RTL8168 driver no longer does %r' % needle


# struct net_info, by offset: the harness reads it out of the guest's own memory, so the numbers below are the
# stack's own counters and not a log line paraphrasing them.
NET_INFO = {'state': 4, 'link': 40, 'lease': 48, 'ip': 52, 'mask': 56, 'gateway': 60, 'dns': 64,
            'rx': 76, 'tx': 80, 'arp_requests': 96, 'arp_replies': 100, 'icmp': 104, 'tcp': 112}


def _net_info(g):
    g.call('net_get_info', g.scratch)
    raw = g.debug.read(g.scratch, 132)
    return {name: struct.unpack_from('<I', raw, offset)[0] for name, offset in NET_INFO.items()}


def test_the_stack_runs_on_emulated_hardware_and_gets_a_lease_from_its_dhcp_server():
    """The model of the wire proves the stack's parsing; it cannot prove the driver, and the model of the card
    proves the registers; it cannot prove the two together.  QEMU's RTL8139 with its user-mode network is the
    one place both run against something that behaves like hardware: the card DMAs real frames, the DHCP server
    is a real DHCP server, and the gateway answers an echo request.  This test boots that machine and reads the
    stack's own counters out of its memory."""
    if not (ROOT / '.tools' / 'qemu' / 'bin' / 'qemu-system-x86_64').exists():
        raise AssertionError('QEMU bundle missing; run tools/setup_qemu.py before this suite')
    from qemu_support import Guest
    with Guest('net-hw', nic='rtl8139') as g:
        deadline = time.time() + 30
        info = _net_info(g)
        while not info['ip'] and time.time() < deadline:
            time.sleep(1)
            info = _net_info(g)
        assert info['ip'] == 0x0a00020f, 'no lease from the emulated DHCP server: %r' % info
        assert info['gateway'] == 0x0a000202 and info['dns'] == 0x0a000203, info
        assert info['link'] == 1 and info['lease'] == 2 and info['state'] == 4, info
        # Two frames out and two in is the whole of a lease: the DISCOVER, the OFFER, the REQUEST and the ACK.
        # A driver that reads the wrong pointer in its own receive ring keeps re-reading the same buffer, and
        # the counter that shows it is the frame count, not the lease.
        assert info['rx'] <= 8 and info['tx'] <= 8, 'the receive ring is being re-read: %r' % info
        # An echo to the gateway: ARP for 10.0.2.2, its reply, then ICMP in both directions.  Typed into the
        # terminal, because the debugger calls functions with interrupts masked, and this stack measures time
        # with the tick.
        g.launch('terminal')
        time.sleep(1)
        g.type('ping 10.0.2.2\n')
        g.run(4)
        after = _net_info(g)
        assert after['icmp'] >= 1, 'the echo was never answered over the emulated card: %r' % after
        assert after['arp_requests'] == 1 and after['arp_replies'] == 1, after
        assert after['rx'] <= 12 and after['tx'] <= 12, 'the counters grew by more than the exchange: %r' % after


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for name, fn in tests:
        fn()
    print('PASS: the stack against the modelled wire (%d checks), the adapter contract the Realtek drivers '
          'implement, the stack and a driver together on QEMU\'s emulated RTL8139 and its DHCP server, the '
          'kernel tick it runs on, and the window and taskbar that report it' % NET_CHECKS)


if __name__ == '__main__':
    main()
