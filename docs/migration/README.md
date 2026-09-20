# SCos x86-64 preparation — custom kernel retained

Status: **planning, source research and host-emulator preparation only**, 2026-09-19. No long-mode entry,
64-bit kernel build target, userspace ABI, driver port or imported library has
been added. r43 is now the 32-bit bug-fix release; r42 remains in Git history. Starting the conversion requires
separate approval. This plan does not replace SCos with Linux.

**Conversion gate remains closed:** [the 32-bit audit](../AUDIT-32BIT.md) found
storage defects now corrected and tested in [r43](../RELEASE-r43.md). Final
physical acceptance and explicit conversion permission remain outstanding.
See the [wider hardware/browser comparison](HARDWARE-AND-BROWSER.md) and
[obtained QEMU host tool](EMULATOR.md). The canonical image/provenance live in
`dist/scos-32bit.img` and `docs/milestones/scos-32bit.json`. Update them for
verified 32-bit fixes until 64-bit starts; then freeze and preserve the final
32-bit image permanently. Do not replace it with a 64-bit image.

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

## Source audit and required changes — NOT implemented yet

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

These are design requirements, not new compiled headers:

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

## Toolchain preparation

Current build uses GCC 12.2.0, GNU binutils 2.40 and Python, targeting i386.
A dedicated `x86_64-elf-gcc` toolchain is not yet provisioned. The host compiler's
availability is not a cross-toolchain readiness guarantee. A standalone compiler
probe (not OS code) produced an ELF64 AMD64 object with 8-byte pointers; this
verifies code generation only. **QEMU 11.0.2 is now obtained and running** through
a pinned third-party musl package/local loader; see [provenance](EMULATOR.md).
It booted the unchanged 32-bit image and exercised guest ACPI power-off.
This is not a 64-bit SCos boot or a physical motherboard shutdown result.

Before conversion, provision a version-pinned `x86_64-elf` GCC/binutils toolchain
(or reviewed Clang/lld cross configuration), assembler and ELF inspection tools.
Build it outside the source tree; record source hashes and configure options.
Use separate i386 and x86_64 output directories. The future kernel build needs
freestanding/no-host-libraries flags, `-mno-red-zone`, an explicit code model,
16-byte C call alignment, and a deliberate FPU/SIMD state policy. Userspace
flags are a separate configuration. Never link host glibc into the kernel.

## Ordered milestones and acceptance gates

**Gate A — now:** finish 32-bit safety corrections and verification, then the
user's physical test/fixes/retest and final acceptance. Update the canonical
32-bit image with verified patches; freeze it only when conversion starts. Planning does not pass this gate.

**Gate B — explicit instruction:** only after acceptance and the user's separate
conversion authorization may long-mode/kernel conversion start. Reset the new
architecture's round to r1 at that point, not now. Reserve `dist/scos.img` for
those future x86-64 images. The existing custom kernel/desktop remains the goal.

**Gate C — after conversion:** restore and validate the converted core before
asking for permission to add drivers/resources. New GPU/NIC/Wi-Fi/browser/libc
imports are NOT implicitly approved by Gate B. The later milestones below are
proposed order only and remain blocked until this separate instruction.


1. **Approve boot/ABI plan and exact hardware targets.** Resolve the blockers
   below; select release versions after license/security review.
2. **64-bit boot only.** Memory map, serial/panic output, framebuffer and
   exception handling; malformed boot data fails safely. No browser work yet.
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
   a complete modern GPU stack.

For each milestone record real resource use, boot behavior and acceptance
results. No invented RAM/CPU/compatibility estimates. Retiring the old tools is
not a waiver of verification for future changes; the r41 checks remain in Git
history, not in the current release tree.

## Blockers / information still needed

* Dedicated GPU vendor/device/subsystem IDs and model; wired/wireless NIC PCI
  IDs or USB VID/PIDs, plus the display connector and enabled iGPU/dGPU setup.
  The i5-11400 CPU does not identify the installed dedicated GPU or NIC.
* BIOS/CSM versus UEFI requirement for the next image, storage controller and
  intended persistent boot medium. Do not assume legacy ATA reaches USB storage.
* Which websites/browser features define success. A small browser for documents
  is a different target from Chromium-class web applications and video/DRM.
* SCos project license/redistribution policy, upstream component license review,
  firmware redistribution rights, version pins and security update ownership.
* Approved bootloader, userspace ABI, toolchain and a repeatable 64-bit boot
  verification environment. Neither full hardware support nor completion of
  these gates is claimed by this preparation package.

See [COMPONENTS.md](COMPONENTS.md) for the researched shortlist and
[candidates.json](candidates.json) for exact research reference commits.
