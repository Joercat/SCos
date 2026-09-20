# Source reuse shortlist — not imported or integrated

Reviewed 2026-09-19. The ranking below is an **engineering recommendation**, not
measured SCos memory usage, a promise of compatibility, or a finished port.
`candidates.json` records exact upstream research commits, not approved shipping
versions. Recheck release tags, advisories and licenses before downloading a
shipping dependency. Keep upstream code separate from SCos adapters and track
local patches; do not rewrite whole libraries to conceal missing OS services.

The expanded [hardware and browser comparison](HARDWARE-AND-BROWSER.md) covers
auto-selection/fallback, Intel/AMD/NVIDIA, wired and wireless driver families,
and genuinely integrated browser engines. Host QEMU is now obtained separately;
no guest dependency has been imported. The [storage audit](../AUDIT-32BIT.md)
blocks final 32-bit acceptance and therefore conversion.

## Recommended first candidates

| Area | Candidate and reason | SCos adapter / prerequisites | License review |
| --- | --- | --- | --- |
| ACPI | **uACPI**: explicitly portable, includes AML, table/region/event and sleep support; supports 32- and 64-bit hosts. Prefer its focused integration API over implementing more AML by hand. | Physical mapping, I/O/PCI access, allocations, timing, interrupt and work/locking services. Integrate namespace initialization and real sleep preparation, not just table lookup. | Upstream MIT; retain notices. |
| TCP/IP | **lwIP**: designed as a small independent stack. Start with raw API/mainloop integration; add socket support only when the threading/backend contract exists. | NIC driver is still required. RX/TX `netif` bridge, `pbuf` ownership, timer service, serialized stack access, entropy and config. `NO_SYS=1` does NOT provide the threaded netconn/socket APIs. | Upstream describes a BSD license; inspect exact files. API metadata alone was inconclusive. |
| Userspace libc | **mlibc**: explicit per-OS `sysdeps` ports and optional POSIX/Linux API groups make it a candidate for a custom OS rather than Linux binary emulation. | Implement a SCos syscall backend, mmap, descriptors, clocks, TLS/errno, threading and synchronization. Its implementation/build needs a suitable C++ toolchain. Choose a pinned release/commit, not moving master. | Inspect actual source licenses; metadata inconclusive. |
| Minimal C runtime alternative | **picolibc**: narrow OS requirements for early standalone C programs. Consider only if the immediate milestone does not require a substantial POSIX layer. | Allocation and I/O hooks, startup and ABI. It is NOT a substitute for a socket API, process loader or scheduler. Do not maintain two competing userspace C ABIs by accident. | Mixed permissive source licenses plus separately licensed tests/support material; inspect `COPYING.picolibc`. |
| TLS | **Mbed TLS**: configurable portable crypto/TLS implementation with upstream porting documentation. | Real entropy, allocation, time, secure persistent trust roots, socket/BIO glue and a maintained configuration. Audit PSA/dependency requirements for the selected release. Never stub entropy or disable certificate verification to get a demo working. | Upstream README states Apache-2.0 OR GPL-2.0-or-later unless a file says otherwise; review transitive dependencies and chosen terms. |
| HTTP/URL transfers | **libcurl**: reuse transfer handling rather than writing redirects, HTTPS and protocol edge cases in the browser. Build only required protocols/features. | libc/socket/readiness/DNS interfaces plus a configured TLS backend. No executable `curl` CLI is required for browser integration. | Review upstream `COPYING` and optional linked dependencies separately. |
| First graphical browser | **NetSurf**, framebuffer frontend: C implementation, explicit small-footprint/portability goals, and its own rendering engine. This is the initial research choice instead of trying to port Chromium/WebKit first. | NetSurf frontend/libnsfb surface and input integration; libc, URL/TLS/network services; matching NetSurf libraries such as libdom, libcss, hubbub, parserutils and image dependencies selected from upstream build instructions. Browser must run isolated from the kernel. | Upstream describes GPL version 2; bundled/separate libraries require their own review. No import until the SCos distribution model is compatible. |
| Fonts | **FreeType**, optional after basic browser rendering: avoids inventing a TrueType rasterizer. | Allocator, file/font asset access and display surface integration. Use licensed font files; don't assume the font renderer license licenses every font. | Inspect FreeType's licensing options and each font's redistribution terms. |
| Future boot handoff | **Limine**, evaluation only: outsource firmware/long-mode boot plumbing while retaining the SCos kernel and desktop. | Versioned boot protocol, memory-map reservations, framebuffer/RSDP handoff and explicit BIOS/UEFI image layout. Not installed and not selected as an irreversible replacement for the current loader. | Upstream repository metadata identifies BSD-2-Clause; verify imported release notices. |

NetSurf's framebuffer frontend is deliberately portable, but a framebuffer
frontend is **not** an entire OS compatibility layer. The acceptance target must
be specified: HTML/CSS pages, forms and verified HTTPS first. Do not promise
full modern web-app compatibility, media DRM, WebRTC or WebGPU. Evaluate actual
sites and JavaScript/DOM support in the selected build before expanding scope.

## Device drivers: select hardware before selecting a port

### Wired NIC first

Do not choose RTL8139 or an emulated Intel card just because it is easy to test
and then claim the user's PC is supported. First obtain actual PCI vendor/device
and subsystem IDs. Compare a narrowly scoped driver from **iPXE**, **FreeBSD** or
**NetBSD** for that device against its host-interface dependencies.

* iPXE is a source/reference candidate, not a drop-in SCos networking subsystem.
  Its PCI, DMA, interrupt, I/O-buffer and device-model dependencies need adapters.
* BSD source can have useful individual driver implementations, but bus-space,
  bus-DMA, locks, callouts, work queues, ifnet/mbuf and PCI services are still
  required. A source file's small size does not measure its dependency graph.
* Preserve license notices and review copyleft/firmware terms **per selected
  driver**, not just the repository's headline license.
* Wi-Fi is deferred: authentication, regulatory rules and firmware considerably
  expand the initial target. It is not a prerequisite for a wired first browser.

### Graphics

There is no credible universal lightweight drop-in accelerated driver for an
unspecified modern dedicated GPU. **Keep software framebuffer rendering first**
(VBE on the current BIOS path; evaluate GOP/bootloader framebuffer for the future
UEFI path). This retains SCos's appearance without blocking initial web access.

Mesa supplies graphics API implementations and hardware/software driver options,
not a replacement for the kernel's PCI/DMA/VM/display infrastructure. A hardware
port requires the relevant kernel-facing interfaces, memory management,
synchronization, command submission and often licensed firmware. Zink still
needs Vulkan; VirGL targets virtual GPUs; neither magically drives the physical
card. LLVMpipe adds LLVM/JIT machinery, so it is not the first small-footprint
choice. Evaluate a specific GPU path only after IDs and the platform contracts
are known. Do not start a giant Linux DRM compatibility layer under the label
of a small driver port.

## Import policy and preparation exit criteria

For each approved component, record: exact source URL and release/commit,
archive SHA-256, complete licenses/notices, dependency closure, required host
APIs, patch list, security advisory review and update owner. Test the unmodified
upstream build separately before adding a narrow SCos adapter. Keep downloaded
source archives and build outputs outside Git until intentionally vendored.
No dependency source, firmware, binary package or library port is included in
the guest or committed as a shipping dependency. The ignored host-only QEMU
runtime and its bootstrap are documented separately in [EMULATOR.md](EMULATOR.md).
In particular, do not assume an Apache-2.0 dependency is compatible with a
GPL-2.0-only browser: review the actual licenses and any compatible dual-license
option or choose another backend before combining distributions.

Preparation is sufficient to choose the sequence, not to claim all blockers are
resolved. Hardware IDs, ABI/toolchain approval and license decisions remain
explicit gates in [README.md](README.md).

## Primary sources

* [uACPI README and integration headers](https://github.com/uACPI/uACPI)
* [lwIP upstream](https://github.com/lwip-tcpip/lwip) and [NO_SYS mainloop documentation](https://www.nongnu.org/lwip/2_1_x/group__lwip__nosys.html)
* [mlibc README / porting guidance](https://github.com/managarm/mlibc)
* [picolibc README and license inventory](https://github.com/picolibc/picolibc)
* [Mbed TLS upstream / porting links](https://github.com/Mbed-TLS/mbedtls)
* [curl build/dependency documentation](https://curl.se/docs/install.html)
* [NetSurf project goals and framebuffer frontend](https://www.netsurf-browser.org/about/)
* [NetSurf source/build instructions](https://github.com/netsurf-browser/netsurf)
* [FreeType upstream](https://github.com/freetype/freetype)
* [Limine upstream](https://github.com/limine-bootloader/limine)
* [iPXE source](https://github.com/ipxe/ipxe), [FreeBSD source](https://cgit.freebsd.org/src/tree/sys/dev), [NetBSD source](https://github.com/NetBSD/src/tree/trunk/sys/dev)
* [Mesa platforms/drivers](https://docs.mesa3d.org/systems.html)

Search results alone were not treated as evidence of a component's SCos
compatibility. No upstream benchmark is presented as a measured SCos result.
