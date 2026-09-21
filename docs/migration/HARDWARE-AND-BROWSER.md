# Wider hardware and browser plan

Research date: 2026-09-19. **Recommendations, not implemented support.** No guest
library, firmware or driver is being imported. The user has authorized conversion;
see the [first startup foundation](BOOT64.md). GPU integration is now authorized; browser integration remains deferred.
The current build implements read-only display discovery and CPU-fallback
notifications only, not accelerated rendering or working-driver selection. The [32-bit audit](../AUDIT-32BIT.md) findings were
corrected in r43 before its final image was frozen.

## Graphics: automatic selection without losing the screen

**2026-09-20 clarification:** use separate reused drivers for different GPU
families, loaded selectively; do not restrict SCos to one PC or write new hardware
drivers. See the
[lightweight 2D/scanout survey](GPU-BASIC-2D-RESEARCH.md) and the wider
[pinned-source driver and loading audit](GPU-DRIVER-RESEARCH.md)
for smaller candidates, measured scope, licensing gates and verification status.

Proposed policy:

1. Enumerate all PCI display-class functions, including render-only devices.
   Record vendor/device/subsystem/revision IDs, BARs, connected outputs when
   discoverable, and the boot display. Intel `8086`, AMD `1002` and NVIDIA `10de`
   identify vendors, **not sufficient driver compatibility**.
2. Match an explicit supported-device table and firmware/version requirements.
   Track distinct states: detected, driver matched, initialized, usable display,
   and acceleration working. A name in a PCI database is not a working driver.
3. Prefer a tested driver which can keep the connected display usable and meets
   the requested rendering features. The largest/faster dedicated GPU is not
   automatically the right display device. Hybrid machines may scan out through
   the iGPU while rendering on the dGPU; cross-device buffer sharing is a later
   feature, not an assumption.
4. Keep the validated firmware framebuffer and CPU renderer available before
   taking over a device. Bound initialization and command-completion waits;
   quarantine a failing driver rather than repeatedly resetting the GPU.
5. On unsupported hardware, missing firmware or failed initialization, use CPU
   rendering and issue a visible, truthful warning, for example:

   > GPU acceleration unavailable — using CPU software rendering.
   > Display output uses the firmware framebuffer. Reason: unsupported PCI ID.

   The GPU still scans out the image; saying it is not used **at all** would be
   inaccurate. Expose the actual device, driver, renderer and failure reason in
   Settings/service logs. Do not invent utilization or acceleration metrics.
6. A destructive reset/modeset may invalidate the firmware framebuffer. Require
   rollback/restoration before claiming fallback still works. UEFI GOP cannot
   simply be called again after ExitBootServices. If no valid framebuffer exists,
   use an available safe console/serial path, never an invalid framebuffer write.

### Driver sources and realistic costs

| Path | What to research | Why it is not a small SCos plug-in |
| --- | --- | --- |
| Intel iGPU / discrete | [i915](https://docs.kernel.org/gpu/i915.html), [Xe](https://docs.kernel.org/gpu/xe/index.html), matching Mesa userspace | Exact generation/PCI ID chooses the relevant path. Display power domains, firmware, GEM/VM, queues, synchronization, interrupts and reset handling are substantial. Newer discrete support is not implied by having an Intel CPU. |
| AMD GPU / APU | [amdgpu](https://docs.kernel.org/gpu/amdgpu/driver-core.html), matching Mesa Radeon userspace | ASIC/IP-specific display, VRAM/GMC, GPU VM, PSP/SMU firmware, DMA and render engines. Supporting an APU and every discrete generation is not one generic register driver. |
| NVIDIA open route | Nouveau kernel-side work plus [Mesa NVK](https://docs.mesa3d.org/drivers/nvk.html) | NVK supplies Vulkan, not the kernel display/VM driver. Its documented Linux interfaces must be implemented/adapted; NVK/Zink still need the lower layers. Coverage depends on generation and upstream version. |
| NVIDIA vendor source | [Open GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules#compatible-gpus) | Turing and later; needs matching GSP firmware and NVIDIA userspace components. OS-agnostic portions do not eliminate the Linux-facing adapter. Kernel modules are not a complete freely portable graphics stack. Check source and firmware licenses separately. |
| Virtual GPU | QEMU's standard VGA framebuffer first; virtio-gpu/VirGL later | Useful test targets, not Intel/AMD/NVIDIA physical support. VirGL adds a host/guest protocol and userspace prerequisites. |
| CPU fallback | Existing software compositor; later evaluate the minimum software API renderer needed by apps | Keep this independent of physical GPU acceleration. LLVMpipe carries LLVM/JIT costs; Zink is not a CPU renderer by itself. |

**Recommendation:** design the selection/failure interface first, retain software
rendering, then port and verify the separate existing drivers in bounded stages. A broad
Linux DRM/Mesa compatibility port can greatly exceed the earlier roughly
10,000-line feature budget, even if most source is reused. Do not quietly import
it as a supposedly tiny driver. There is no verified lightweight package that
provides all three vendors' modern acceleration on the present SCos kernel.

## Networking: separate the driver from the stack

Recommended layering: SCos PCI/USB + DMA/IRQ/timers → selected NIC driver →
bounded RX/TX queues and `lwIP netif` → IP/DHCP/DNS/TCP → sockets/TLS/HTTP.
Every buffer needs an ownership/lifetime rule. IRQ handlers must not block the
input/UI loop; polling work needs a budget. A physical link, DHCP lease, routed
IP connectivity, DNS and HTTPS are separate success states.

### Ethernet candidates

| Hardware / purpose | Candidate | Qualification |
| --- | --- | --- |
| Small reference drivers | [iPXE intel.c](https://github.com/ipxe/ipxe/blob/65450656e502c2456b9b32b9dff09152f2a754b8/src/drivers/net/intel.c), [realtek.c](https://github.com/ipxe/ipxe/blob/65450656e502c2456b9b32b9dff09152f2a754b8/src/drivers/net/realtek.c) | Reviewed files are 1,231 and 1,275 lines respectively, **not total port sizes**. Attractive freestanding references, but still require iPXE device/PCI/DMA/buffer adaptations and exact ID review. Selected files offer GPL-2.0-or-later or UBDL; do not assume the unmodified-binary alternative permits source adaptation on the same terms. |
| Intel gigabit | OpenBSD [em](https://man.openbsd.org/em.4) | Broad documented 825xx/I210/I211/I217/I218/I219/I350 families; match exact IDs and revisions. |
| Intel 2.5 GbE | OpenBSD [igc](https://man.openbsd.org/igc.4) | I225/I226; not automatically covered by an older e1000 driver. |
| Realtek gigabit / older devices | OpenBSD [re](https://man.openbsd.org/re.4) | Several 8139C+/8169/8168/811x families; not every Realtek NIC. |
| Realtek newer multi-gigabit | OpenBSD [rge](https://man.openbsd.org/rge.4) | Current manual lists RTL8125/8126/8127; choose a pinned source revision with the required ID. |
| USB Ethernet | OpenBSD [ure](https://man.openbsd.org/ure.4) | RTL8152/8153/8156/8157 family support varies by revision. Requires general USB bulk/control transfers, not merely SCos's working HID path. |
| Virtual test NIC | virtio-net, then an emulated e1000-family target | Deterministic development coverage; successful emulation does not establish physical PHY/reset/DMA compatibility. |

**Recommendation:** lwIP plus one exact wired NIC first; compare iPXE's narrow
freestanding implementation against the appropriate BSD driver's dependency
closure. BSD drivers require bus-space/bus-DMA, ifnet/mbuf adaptation, callouts,
locks and PHY services. Review each file's license. Do not promise universal
coverage or benchmark/resource numbers without a built port on actual hardware.

### Wi-Fi is a second, larger subsystem

Wi-Fi additionally needs scanning, association, regulatory/channel enforcement,
key installation, authentication, firmware loading and recovery. `lwIP` does
not provide these, and [wpa_supplicant](https://w1.fi/wpa_supplicant/) does not
replace the hardware driver. The supplicant needs an OS/driver control backend,
crypto, timers, entropy and its own supported security configuration.

| Candidate | Useful scope | Main caveat |
| --- | --- | --- |
| OpenBSD [iwx](https://man.openbsd.org/iwx.4) | Intel AX200/210 and AX201/211 CNVi families | Firmware and net80211 services; CNVi is platform-dependent. The manual explicitly does not claim 802.11ax capabilities. Do not infer WPA3 or Wi-Fi 6 functionality from the adapter's name. |
| OpenBSD [athn](https://man.openbsd.org/athn.4) | Older Atheros 802.11n PCI/PCIe and selected USB devices | Narrower generation coverage; USB variants need firmware. Potentially a more bounded first experiment if this is the user's actual adapter. |
| Linux [wireless drivers](https://wireless.docs.kernel.org/en/latest/en/users/drivers.html) | iwlwifi, ath9k/10k/11k/12k, mt76, rtw and other families | Broad ecosystem, but cfg80211/mac80211, kernel services and firmware are not small incidental dependencies. Choose by ID, bus and required features. |
| FullMAC reference: OpenBSD [bwfm](https://man.openbsd.org/bwfm.4) | Selected Broadcom/Cypress PCI/USB/SDIO chipsets | Firmware handles some MAC work, not all host networking/security. The current manual warns of outdated firmware with known vulnerabilities; **not a default recommendation** without resolving that security issue. |

Start with a specified adapter and station mode. Validate WPA2-CCMP at minimum;
add WPA3 only with an actually supported driver/stack/firmware combination.
Do not substitute WEP, obsolete WPA/TKIP or disabled certificate validation for
missing functionality. Preserve regulatory restrictions. Firmware redistribution
rights, available secure versions, and update ownership are approval gates.

## Integrated HTML + CSS + JavaScript engines

An integrated engine supplies parsing/layout, DOM and JavaScript bindings. It
still needs SCos graphics/input, fonts/images, time, files, networking/TLS and an
isolated process/runtime. A bare JS interpreter is not a browser.

| Engine | Integrated? | SCos assessment |
| --- | --- | --- |
| [NetSurf](https://www.netsurf-browser.org/about/) framebuffer frontend | HTML/CSS, DOM and Duktape-based JavaScript support | **First lightweight candidate.** C, portability-oriented frontend, relatively bounded platform integration. Compatibility is deliberately more limited than a mainstream browser; test the desired sites, DOM APIs and scripts, not just whether JS executes. GPLv2 browser plus separately reviewed dependencies. |
| Serenity [LibWeb](https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibWeb) + LibJS | Yes, with Serenity's supporting libraries/services | Strong hobby-OS reference and broader engine ambitions. Not a standalone two-library drop-in: AK, LibCore, IPC, crypto/TLS, image/font/graphics and other dependency edges must be mapped. C++ runtime and OS interfaces are substantial. Review exact BSD-style source licenses and third-party notices. |
| [Ladybird](https://github.com/LadybirdBrowser/ladybird) | Yes, separate project descended from Serenity's browser | Larger modern-browser path, not interchangeable with current Serenity libraries. Platform services and multiprocess architecture make it a later option, not a small first port. Independently review current licenses/build dependencies. |
| [Servo](https://github.com/servo/servo) | Yes, embedding-oriented web engine with SpiderMonkey | Rust plus C/C++/JS-engine dependencies and rendering/platform services. MPL-2.0 project plus dependency licenses. Being an embedding library does not make it freestanding or establish a small SCos footprint. |
| [Ultralight](https://ultralig.ht/) | Yes, WebKit/JavaScriptCore-derived, CPU and GPU renderers | Relevant feature set, but advertised SDK targets established OSs/consoles. Full source is proprietary/commercial; only part of WebCore is available under LGPL. A Linux binary cannot simply link into SCos. Not the open-source first choice without an explicit custom-platform/source-access arrangement. |
| [litehtml](https://github.com/litehtml/litehtml) + [QuickJS](https://bellard.org/quickjs/) | **No** | litehtml supplies HTML/CSS layout and expects drawing/font/image callbacks; QuickJS supplies JavaScript language execution. DOM bindings, browser APIs, events and layout integration remain a separate engine project. Useful for constrained local UI, not an honest all-in-one modern browser recommendation. |

**Recommendation:** define a small real site/document acceptance corpus, evaluate
NetSurf first, keep LibWeb/LibJS as the next larger source-reuse study. Do not
claim modern websites, media/DRM or browser security until demonstrated. Keep
untrusted web parsing and JavaScript out of the kernel; begin with local HTML,
then HTTP, then correctly validated HTTPS. Engine integration waits for the
separate post-conversion approval, not merely a 64-bit compiler switch.

## Information needed before integration approval

* GPU and wired/wireless NIC PCI IDs (including revision/subsystem); USB VID/PID
  where relevant. GPU connector in use and whether both GPUs are enabled.
* Actual boot mode, storage controller and intended persistence medium. USB
  boot through BIOS does not imply a native USB mass-storage driver.
* Required Wi-Fi authentication/AP capabilities and acceptable firmware terms.
* Specific browser sites/features, distribution license and security-update plan.

No need to guess any of this to finish planning. It is needed before promising
hardware compatibility or selecting the first port.
