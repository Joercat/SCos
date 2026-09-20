# Original platform drivers — native x64 integration

Status: unnumbered development, 2026-09-20. The user's instruction was to check
the converted components together first, then finish the remaining **original**
drivers and check their integration. This is not physical-PC acceptance or a
claim that every app workflow and every hardware failure has been tested.

## Provenance and scope

`kernel/drivers/{pci,usb,ata,acpi}.c` adapts the individual original sources from
`6717943f977a7e0f95f0ace5fa48cfe6a564f873:legacy/i386/kernel/src/`.
No Linux kernel/driver, new GPU/network/browser library or obsolete i386 build
was imported. PCI access/enumeration was refactored for atomic config access,
correct multifunction traversal, bridge-cycle protection and native BAR sizing.
The larger USB, ATA/FS2 and ACPI implementations retain their original logic.
Eleven original HID functions were compared byte-for-byte: `hid_rx_bytes`,
`hid_bits`, `hid_signed`, `hid_parse_layouts`, `hid_set_frames`,
`hid_frame_packet`, `hid_inject_one`, `hid_inject_layout`, `mouse_probe`,
`usb_hid_parse`, and `hid_fill_ep_ctx` are unchanged. In particular, the accepted
r41 packet-sized requests, fragmented reports, report IDs and wide axes were not
replaced with a boot-mouse-only shortcut or sensitivity guess.

## Native boundaries and corrections

* PCI mechanism-1 accesses are interrupt-atomic on the current single CPU.
  Command-register changes use 16-bit writes, not a status/W1C DWORD RMW.
  Only class 0c/03/30 is treated as xHCI. BARs keep firmware placement, including
  above 4 GiB; the old guessed 32-bit relocation hole is gone.
* Memory services map device registers UC/RW/NX, refuse RAM aliases and bound
  mappings to the native physical range. ACPI reads require mapped ACPI
  reclaim/NVS ownership and RO/NX mappings. The existing allocator gains an
  exclusive address-limit parameter without creating a second RAM allocator.
* xHCI structures use page-aligned, checked DMA32 pages, not heap payloads.
  DMA32 is intentional and works with both AC64 and non-AC64 controllers;
  CPU/MMIO pointers remain 64-bit and event pointer high halves are checked.
  Control transfers use per-slot bounce pages, never a stack/high-memory DMA
  pointer. Allocation failures stop initialization/enumeration safely.
  Scratchpad sizing uses both architectural count fields. Capability/register
  offsets, page-size support, halt/reset/readiness and firmware ownership are
  checked before initialization proceeds. Doorbells have publication barriers.
* Completion matching includes the submitted TRB address. Timed-out control
  buffers are quarantined until slot teardown. A command timeout disables
  further commands rather than recycling potentially DMA-owned storage.
  Disable Slot must complete before its DMA pages are freed; otherwise pages
  remain retained. Hub teardown includes descendants and releases held input.
  Kernel memory reporting now includes actual USB DMA allocation bytes.
* Root-port tests exposed two separate old hotplug defects: Port ID was read
  from event DWORD 3 instead of DWORD 0, and disconnects were discarded when
  CCS was clear. Both are fixed. Change acknowledgments retain port power.
  Reconnects within the one-second work interval retire the old slot too.
  Status is counted from live slots, not frozen boot-time device counts.
* The external-hub test initially failed Address Device with completion code 5.
  Clearing a USB2 child's route aliased its already-addressed parent in QEMU.
  Route information is now retained at all speeds, including Configure Endpoint.
  This agrees with Linux's unconditional `udev->route` slot-context setup;
  it is not a QEMU-specific fallback. References inspected for the diagnosis:
  [QEMU v11.0.0 xHCI](https://github.com/qemu/qemu/blob/v11.0.0/hw/usb/hcd-xhci.c)
  (`xhci_lookup_uport`, Address Device's already-assigned-port check) and
  [Linux v6.19 xHCI memory setup](https://github.com/torvalds/linux/blob/v6.19/drivers/usb/host/xhci-mem.c).
* USB polling runs in WM/TTY foreground loops, not the IRQ call graph. The
  RTC-bounded nonfatal error loop drains USB events without blocking hotplug
  probes. The existing SIMD-free native interrupt boundary remains intact.
* ACPI uses the UEFI-provided RSDP, not EBDA/BIOS scans. Table addresses are
  native-width; signatures, ownership, lengths and checksums are checked.
  The original constant `_S5_` parser, PM1 I/O controls, SCI enable and manual
  power-off fallback remain. This is not a full AML interpreter.

## Persistence and destructive-operation safety

The 64-MiB disk remains GPT/FAT32/UEFI, but the ESP now ends at LBA 129023.
A separate SCos-data partition occupies **LBA 129024–129279 (128 KiB)**. Both GPT
copies describe it. Sector 0 has the explicit `SCOSDATA64v1` ownership marker;
this is an ownership convention, not cryptographic authentication.

Legacy ATA PIO is used only where PCI advertises enabled compatibility-mode IDE
channels. A unique target must have the ownership marker, protective MBR,
expected partition types/bounds/attributes, primary and backup GPT headers and
matching entry arrays with valid CRCs. Additional partitions, multiple matching
drives or unreadable candidate metadata prevent target selection. The original
32-bit image and the former all-ESP x64 image are **not** writable save targets.
Before each save, ownership/GPT validation runs again. Writes are confined to
the data partition; the ESP, boot files and GPT are never save destinations.

The original bounded FS2 serializer, CRC, flush and readback verification remain.
The staging buffer and restored root have stable VFS ownership rather than the
calling app's lifetime. Loading a malformed/incomplete tree retains RAM defaults;
required desktop directories must exist. Saving an oversized tree refuses it.

**Save remains explicit**: Notepad and Settings update RAM; run `save` in a shell
and confirm to persist the tree. Settings Factory Reset confirms, replaces the
RAM tree, clears the entire owned saved-data region while writing defaults,
verifies it, then reboots. It is **not whole-disk secure erasure**. Without a
verified target it says RAM-only and leaves disks alone. A failed persistent
reset does not proceed to reboot.

FS2 invalidates the first sector before writing the remaining image and commits
it last. This detects interrupted writes; **it is not a crash-atomic journal**
and does not guarantee recovery of the previous tree after power loss.

## Executed verification

Pinned QEMU 11.0.2/EDK2, TCG, one vCPU; only disposable regular image files,
no physical disk/device passthrough or guest network. QMP drove real emulated
keyboard/mouse events. Temporary GDB inspection checked native VFS contents,
window/console state and DMA allocations; selected failure/API cases called the
actual guest functions. These function calls are distinguished from UI tests.

| Stage / test | Observed result |
| --- | --- |
| Before driver edits: published app baseline, 128 and 256 MiB | Native boot, keyboard-driven mkdir/touch, all ten app windows together, confirmed WM kill and fresh desktop return passed. This was a fresh integration smoke test, not a rerun of every older APP64 scenario. |
| Combined native drivers/apps, q35 128 MiB; PC/IDE 256 MiB | All ten apps open together, RAM VFS commands work, WM kill/restart works. Inspected screenshot showed original Solitaire/card rendering and the stacked desktop/SysMon/taskbar. |
| q35 xHCI MMIO at `0x000000c000000000` | Above-4-GiB register mapping, keyboard/mouse enumeration and actual USB input work with DMA32 structures. |
| Notepad through real USB keyboard, PC/IDE | Created a document, typed `native usb storage`, Ctrl-S, then shell `save`: cancellation left the blank data partition unchanged; confirmation saved it. A new guest loaded identical bytes. |
| Disk-byte comparison | All bytes outside the owned 128-KiB data partition matched the original image; FS2 CRC matched. |
| USB mouse and ring wrap | Repeated relative X/Y reports moved the compositor pointer correctly; repeated keyboard input crossed HID/event ring boundaries with no ring-full count. |
| Root hotplug | Four disconnect/reconnect cycles accepted subsequent file-creation commands with unchanged DMA allocation count. Three rapid, sub-heartbeat reconnects also passed with stable DMA bytes. |
| External emulated USB hub | Hub port configuration/control-ring wrap completed; keyboard on downstream port accepted a real TTY file-creation command. |
| Unsupported UHCI | Not treated as xHCI; no xHCI initialization attempted. |
| Six consoles | Ctrl-Alt-F1–F6 selected independent sessions; `calc 6 - 2` produced `Result: 4` in each; each created its own VFS file. F7 returned to WM. |
| Memory / high RAM | 2048-MiB guest with `max-ram-below-4g=1G` booted, used USB and launched Solitaire. Guest API probes checked contiguous DMA32 range/alignment, release, rejection of MMIO-over-kernel-RAM and out-of-range mappings. Kernel memory attribution included DMA bytes. |
| Unowned disk / duplicate owned disks | Save target unavailable; direct save refusal and unchanged image bytes verified. |
| Corrupt saved tree | Defaults retained, corruption logged, disk untouched. |
| GPT damaged after firmware handoff | Damaging either primary entries or backup entries made the next native save refuse ownership; disk stayed unchanged thereafter. Firmware can repair a damaged primary GPT before the kernel boots, so this test deliberately damaged metadata after handoff. |
| Actual Settings UI Factory Reset | USB mouse opened the confirmation; Escape preserved the file. A second click/Enter wiped saved user content and rebooted. A fresh guest loaded verified defaults with the old document absent. Other disk sectors stayed unchanged. |
| Power | Confirmed shell shutdown terminated both PC and q35 guests through ACPI S5. Cancellation kept the guest running. Confirmed 8042 reboot worked. Setting the guest ACPI-available flag false exercised the logged manual-power fallback without exiting. |
| Panic | Real USB TTY `sysrq panic integration` reached the native panic path independently of WM. |
| Build / provenance | Warning-as-error native build, deterministic clean rebuild, ELF/PE image validation, frozen-image guard and Git diff checks. Published-byte verification is recorded in the delivery response. |

Temporary test scripts, disposable disks, screenshots, reference downloads and
logs are retired after verification; the legitimate pinned host QEMU tools stay.
No temporary diagnostic guest commands/UI or v86 were added.

## Explicit remaining limits

* No physical i5-11400/GPU/USB-device acceptance yet; QEMU is not that hardware.
  No new release number. Existing app windows still share one cooperative ring-0
  execution context: no process isolation, preemptive scheduling or SMP.
* Original xHCI scope is retained: first matching controller, up to 15 root ports,
  12 slots, three HID endpoints per device, 64-byte receive buffers; USB2 hub
  walking is bounded (eight ports, limited depth). No OHCI/EHCI/UHCI, USB mass
  storage, SuperSpeed-hub driver or simultaneous multi-controller support.
  Downstream hub-child hotplug while HID is active remains unsupported: the
  original blocking hub-health probes stay suppressed to protect working input.
* DMA is identity-addressed, coherent x86 DMA32; there is no IOMMU driver.
  Hardware timeouts, allocation exhaustion and all malicious descriptor/table
  permutations have not been exhaustively fault-injected.
* ATA PIO conversion does **not** add AHCI, NVMe or USB-disk persistence. A USB
  boot or a typical AHCI-only modern PC can run the OS but still have RAM-only
  files. The original 128-KiB saved-tree budget remains.
* Dynamic AML `_S5`, `_PTS`/`_GTS`, hardware-reduced ACPI and non-I/O PM1 controls
  require broader ACPI support. Above-4-GiB table handling was adapted, but this
  matrix did not relocate firmware ACPI tables above 4 GiB.
* New accelerated GPU, Ethernet/Wi-Fi/networking and real browser integrations
  still require separate user permission. The browser remains its original
  unavailable notice. None is implied by finishing these original drivers.

## Follow-up inventory and integration recheck

A subsequent request to continue converting the remaining drivers prompted a
fresh comparison against the original `6717943` source tree. No additional
original hardware-driver implementation was found waiting to be ported:

| Original component | Active native implementation |
| --- | --- |
| PCI, xHCI/HID, ATA/FS2, ACPI | `kernel/drivers/{pci,usb,ata,acpi}.c` |
| PS/2 keyboard/mouse and CMOS RTC | `kernel/desktop/{kbd,mouse,rtc}.c` |
| PIT/PIC/IDT and interrupt dispatch | `kernel/src/interrupt.c`, native vector stubs |
| Framebuffer drawing and firmware display setup | Original drawing in `kernel/desktop/fb.c`; native GOP handoff/console instead of BIOS/VBE |
| Memory and CPU accounting support | `kernel/src/memory.c`, `kernel/desktop/{heap,cpumeter}.c` |

This is a **driver inventory**, not a claim that every historical feature or UI
has been exhaustively checked. AHCI, NVMe, USB mass storage, accelerated GPU,
Ethernet and Wi-Fi would be new implementations/integrations, not additional
original drivers recovered from that tree.

Fresh q35 and PC/legacy-IDE guests booted the unchanged published image with
xHCI keyboard/mouse attached. Real keyboard events launched Solitaire, switched
back to TTY, evaluated `calc 6 - 2`, and confirmed ACPI shutdown. Screenshots
were inspected: Solitaire rendered, TTY showed `Result: 4`, and the PC guest
created `/home/recheck.txt` and reported a successful confirmed `save` on its
disposable snapshot. Both guests exited through guest shutdown. This narrower
smoke recheck does not replace or claim to repeat every row of the earlier
matrix, including persistence across restart and factory reset.

`make`, build/published-image byte comparison, checksum and frozen-image checks
passed. Temporary scripts/screenshots/logs and guest state were removed. No
kernel/image change was required by this recheck. Broader new-driver scope still
needs an explicit choice; hardware-specific networking/GPU work also needs the
actual device models/IDs.
