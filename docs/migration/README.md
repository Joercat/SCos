# SCos x86-64 conversion — custom kernel retained

Status, 2026-09-20: **native x64 UEFI startup implemented; BIOS path removed**.
The subsequently authorized app/shared-desktop port is described in
[APP64.md](APP64.md); it reuses original source and is now active. The user then
authorized the remaining original drivers and combined verification, now recorded
in [DRIVER64.md](DRIVER64.md). This does not authorize new driver/library imports.
See [BOOT64.md](BOOT64.md) for the implemented boot ABI, test results and explicit
limitations. Root `make` is AMD64. Duplicate i386 sources were removed on 2026-09-20;
retrieve individual porting references from commit `6717943` when needed.
`dist/scos-32bit.img` is permanently frozen at r43. `dist/scos.img` is the
unnumbered foundation, not a replacement desktop release. The user deferred
numbering until conversion starts on their PC. New driver/library integration
still requires separate permission after the existing core is converted.

See [hardware/browser research](HARDWARE-AND-BROWSER.md) and
[host QEMU tooling](EMULATOR.md); no researched guest component is imported.

## Baseline and boundaries

The user physically accepted r41 input: repeated keys, stationary clicks and
wheel input work without spamming. Preserve commit
`72d6176445f1f5b66f2dd575de3f38b518ad32c3` and its image as the input reference.
The r42 changes are shutdown repairs and retirement of temporary tooling, not
an architectural conversion. `os.html` remains the desktop design reference.

Reusing source is realistic. Running existing Linux binaries or loading Linux
kernel modules is **not** an automatic consequence of selecting `-m64`.
The adapters, memory model, execution environment and licenses are real work.
We prefer narrow upstream adapters over maintaining large rewritten forks.

## Original i386 audit and remaining conversion work

The paths below refer to historical `legacy/i386/` at commit `6717943`,
not present-tree files. This table records the original audit, not current
bootloader choices. BOOT64 supersedes its boot/paging proposals; APP64 covers
implemented app/shared-desktop work; DRIVER64 covers the original drivers.
Other entries remain a roadmap.

| Current location/assumption | Future requirement |
| --- | --- |
| `boot/stage1.S`, `boot/stage2.S`: BIOS, protected-mode entry, E801 memory size, VBE, flat image | Choose a versioned boot contract and real memory map. Evaluate Limine BIOS+UEFI loading of our kernel rather than extending every firmware path manually. A bootloader is not a replacement kernel. Keep a known-good BIOS image during evaluation. |
| `kernel/entry.S`, `kernel/linker.ld`: entry at 1 MiB, 32-bit interrupt frames | Long-mode entry, paging, canonical virtual addresses, 16-byte IDT gates, 64-bit interrupt stubs, GDT/TSS/IST, correct privilege transitions and stack alignment. |
| `scos.h`: pointers stored in `u32`, `unsigned` string lengths, 32-bit framebuffer address | Separate `uintptr_t`, `size_t`, physical and DMA address types. Keep hardware register/wire fields fixed-width. Do not mechanically replace every `u32` with `u64`. |
| `mm.c`: contiguous allocation below 1 GiB, physical address equals pointer | Reserve firmware/ACPI/boot structures and framebuffer/MMIO regions from a real map; page allocator plus virtual mappings; guard pages and memory permissions; DMA32 pool for devices that need it. |
| `usb.c`: `PA(p)` narrows pointers; TRB address high halves assumed zero | Distinguish CPU virtual pointers from bus addresses. Respect xHCI AC64 capability, populate low/high address halves, use DMA32 where required. Preserve packet-sized receive TDs and report assembly exactly. |
| `fb.c`: boot-only MTRR setup assumes paging off and one executing CPU | Define framebuffer WC and MMIO UC mapping policy; PAT/MTRR interaction; maintain the single-CPU boot ordering until a real SMP-safe implementation exists. |
| `idt.c`, `pit.c`, `cpumeter.c`, keyboard/mouse | New saved-register ABI, interrupt return path and fault-safe stacks. Keep input semantics; do not repeat the transport/sensitivity changes. Retain a simple single-CPU interrupt/timer arrangement initially. |
| `acpi.c`: bounded constant `_S5` parser, I/O PM1 controls below 4 GiB | Integrate an AML interpreter and physical mapping interface later; full firmware method execution is not solved by widening pointers. |
| All apps run in ring 0; terminal process rows mostly represent kernel components | Implement real address spaces, scheduling, ELF64 loading, syscalls, resource ownership and teardown before exposing a network browser to untrusted content. Do not claim process isolation from the existing app table. |
| `lib.c` is a small kernel utility library, not POSIX libc | Keep kernel internals separate from userspace libc. Define one SCos userspace ABI and implement the libc backend against it. |
| `ata.c`: legacy ATA PIO and custom VFS persistence | An image booted from USB does not imply USB mass-storage persistence support. Plan storage drivers, file descriptors, reliable flush and crash behavior separately. |
| `app_browser.c`: stub; no network stack or NIC driver | Socket/event interfaces, actual NIC support, DNS/DHCP, TCP, validated TLS and a browser frontend are separate dependencies. |

Search targets during the future port: `(u32)` pointer casts, `PA(`, `lfb_base`,
`mem_kb`, packed structures, inline assembly, varargs formats, `unsigned` sizes,
interrupt save frames, all DMA pointers and pointer-to-integer round trips.

## Proposed contracts

The broader requirements below extend the implemented minimal boot contract;
framebuffer, RSDP, modules and userspace are not in boot ABI version 1:

* Boot handoff: magic + version + structure size; 64-bit framebuffer address,
  pitch/format; typed memory-map entries; RSDP physical address; module spans.
  Validate sizes and reserve every referenced span before allocation.
* Platform: map/unmap physical memory with explicit cache policy, monotonic
  clock, IRQ registration, locks, sleep/wake, deferred work and allocation.
* DMA: bus-address/CPU-pointer pair, alignment, address mask, mapping lifetime,
  memory barriers and completion ownership. No implicit identity-map casts.
* Networking: bounded RX/TX buffers, link state, MTU, polling/IRQ ownership;
  then sockets, nonblocking I/O, readiness and DNS above the stack.
* Display: framebuffer/surface lifecycle, pitch/format, damage rectangles,
  bounded shared surfaces and input routing. Preserve the SCos compositor,
  app identity, desktop behavior and six-console experience.
* Userspace: ELF64 SysV calling convention plus a documented SCos syscall ABI;
  files, time, VM, threads/synchronization and process exit. Defer dynamic linking
  until static userspace works. Do not expose arbitrary physical memory to apps.

## Toolchain and approval state

The tested build uses host GCC 12.2.0 and GNU binutils 2.40 with explicit AMD64
freestanding/no-host-library flags, no red zone, general-register-only C and
SysV stack alignment (desktop foreground callbacks may use SSE2; IRQ paths may
not). There are no active BIOS stages or `-m32` target. A dedicated
version-pinned cross toolchain remains desirable; it is not falsely claimed to
have been provisioned. Only the AMD64 build remains active.

**Gates A/B are open:** the user reported no major remaining 32-bit blockers
and explicitly requested conversion. The final r43 image is frozen. The first
UEFI boot/ABI implementation is present, not the entire architecture/desktop port.
No round number until conversion starts on the user's PC. **Wait for the user to
choose the next subsystem; this roadmap is not permission to begin it now.**

**Gate C stays closed:** separate permission after conversion is required for
new drivers/resources. The sequence below remains a roadmap, not a list of
completed or approved integrations.

1. **Approve boot/ABI plan and exact hardware targets.** Resolve the blockers
   below; select release versions after license/security review.
2. **Native UEFI startup implemented.** Relocatable kernel, memory map/ownership,
   firmware exit, GOP output, page protection, allocator and exception/IRQ paths;
   see BOOT64 for exact tests and limits. Physical acceptance remains outstanding.
3. **Restore SCos behavior.** Allocator, interrupts, validated HID behavior,
   consoles, WM lifecycle and files. Re-run the r41 packet-boundary cases from
   history plus real-PC repeated input before changing any driver algorithm. This core
   conversion must be finished before the separate integration approval.
4. **After Gate C: protected execution.** Separate userspace mappings, ELF loader, threads,
   syscalls and resource cleanup. A crashing app must not corrupt the kernel.
5. **Wired network and libc.** Real NIC link/RX/TX, ARP, DHCP, DNS and TCP;
   test partial I/O, timeouts, disconnects and retransmission. No fake ping.
6. **TLS and first browser.** Trusted certificate store, trustworthy clock and
   entropy; hostname/chain/time validation. Local HTML first, then HTTP, then
   HTTPS, images/forms/downloads. Invalid certificates must fail closed.
7. **Hardware graphics, only after device selection.** Retain software rendering
   as the working fallback. Browser usefulness must not depend on first porting
   a complete modern GPU stack. Reusable *light* drivers per family are surveyed in
   `GPU-BASIC-2D-RESEARCH.md`; SCos' exact auto-detection and driver-registry layer is
   implemented and verified in `GPU-AUTO-DETECT.md`, which also states that no engine is
   bound yet, so all drawing is still the CPU compositor.

For each milestone record real resource use, boot behavior and acceptance
results. No invented RAM/CPU/compatibility estimates. Retiring the old tools is
not a waiver of verification for future changes; the r41 checks remain in Git
history, not in the current release tree.

## Blockers / information still needed

* Dedicated GPU vendor/device/subsystem IDs and model; wired/wireless NIC PCI
  IDs or USB VID/PIDs, plus the display connector and enabled iGPU/dGPU setup.
  The i5-11400 CPU does not identify the installed dedicated GPU or NIC. Running
  `graphics` in SCos Terminal now prints each display adapter's `vendor:device`, the
  family whose upstream table binds it (or that none does), and the port state, which is
  the shortest path to the numbers needed here.
* Storage controller and intended persistent boot medium. The boot decision is
  already resolved: x64 UEFI only. Do not assume legacy ATA reaches USB storage.
* Which websites/browser features define success. A small browser for documents
  is a different target from Chromium-class web applications and video/DRM.
* SCos project license/redistribution policy, upstream component license review,
  firmware redistribution rights, version pins and security update ownership.
* Future userspace/isolation ABI and remaining driver interfaces. The native
  bootloader, toolchain and host QEMU environment are already implemented.
  Full hardware support and physical acceptance remain outstanding.

See [COMPONENTS.md](COMPONENTS.md) for the researched shortlist and
[candidates.json](candidates.json) for exact research reference commits.
