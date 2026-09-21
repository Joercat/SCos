# Application management and layout verification

Unnumbered x64 development build, 2026-09-20. This follows the compositor update;
the older COMPOSITOR-VERIFICATION.md describes its own historical binary.

## Delivered

- Native Applications manager: launch, pin/unpin and default-no uninstall of
  external CAT apps. Terminal/TTY command: `appuninstall <id>`. Built-ins remain
  protected. Uninstall closes instances, removes the registration and desktop /
  taskbar links, and deletes the registered installed package under home/apps.
  Projects, private app data and original external/demo packages remain. Slots
  are reused rather than exhausting the registration limit after repeated use.
- RAM-only installation/removal explicitly says session-only. Existing supported
  ATA saves remain available, including explicit save-failure reporting; no new
  USB, AHCI or NVMe storage drivers were introduced.
- At most eight taskbar launch shortcuts, with capacity reduced on narrow screens.
  Defaults: Files, Terminal, Studio, Applications. The Windows picker remains.
  Pins live in home/taskbar.txt; ordinary filesystem persistence rules apply.
- Three top-right notification cards composed above windows, menus and modals.
  Normal notices expire after ten seconds; warnings persist until dismissed or
  evicted by queue overflow. Mouse dismissal does not approve underlying modals.
  Text wraps at word boundaries, hard-breaks long tokens, and uses ellipsis when
  four lines are insufficient; full messages are logged. Lua `scos.notify(text)`
  uses the app ID as title, 192-byte limit, and shared five-second message throttle.
- Files keeps stable target IDs/paths separately from display labels. Desktop
  aliases are recognized by VFS node identity, and double-clicking an app shortcut
  opens that app instead of passing its label to Notepad. Synthetic right-click
  offers Open. Protected/unavailable directories have distinct messages.
- Files names/type columns and native app labels are bounded. Calendar title and
  grid, About/SysMon minimum sizes and clipping, Browser content position,
  Blackjack hand spacing, Solitaire compressed tableau paint/hit coordinates and
  footer layout were revised. Notepad reveals long-line carets horizontally and
  accepts body clicks; Terminal reserves input room even for long cwd prompts.
- desktop_load previously parsed delimiters in live VFS bytes. It now parses an
  allocated bounded copy and leaves serialized desktop.json intact.

## GPU request: incomplete

Read-only PCI segment-0 display discovery identifies up to eight devices and
logs vendor/device/BDF. `graphics` reports inventory, firmware scanout and CPU
rendering. Boot displays an explicit CPU-fallback warning. No BAR-size probing,
GPU reset or modeset is attempted just to identify a device.

**There is no accelerated GPU backend, working-driver selection, or hardware
rendering in this delivery.** Broad GPU support remains unfinished. Vendor
identification, GOP framebuffer access and PAT write combining are not GPU
execution. Browser integration remains deferred. No physical-PC performance or
hardware compatibility certification is claimed.

## Final-binary verification

Two clean `make clean && make -j4` builds completed with warnings treated as errors
and produced byte-identical images. Image size: 67,108,864 bytes. SHA256:

```
826a409fe8beb35254f31a27bea37d52aa04dcd3f0f0dfa1dd10dc6fbcd66b0a
```

All four retained suites passed on that binary:

```
python3 tools/tests/test_cat.py
python3 tools/tests/test_studio_edit.py
python3 tools/tests/test_compositor.py
python3 tools/tests/test_management.py
```

CAT/Studio/management exercise USB/xHCI and IDE/PS2 guests. Compositor tests
compare held-interaction partial/full scanout with one and two vCPUs (not an SMP
implementation). Existing package validation, real compiler failures, bounded
clipboard, theme, native lifecycle and persistence tests remain.

New management coverage includes GUI cancellation/approval, actual Terminal y/n,
34 install/uninstall slot-reuse cycles, built-in protection, package deletion and
app-data retention across ATA reboot, non-mutating desktop load, Files Desktop
alias app dispatch and protected-system feedback, narrow/wide pin capacities
and full-capacity rejection. Notification scanout over a modal matches the same
card without that modal; dismissal leaves the modal unresolved. QEMU PCI identity
and truthful CPU fallback are checked. Notepad long-line horizontal reveal, Home
and body-click cursor placement are checked. CAT tests include notify success
and throttle behavior. Application-manager screenshot was visually inspected.
These are emulated tests, not proof of every app geometry or physical device.

Frozen dist/scos-32bit.img remains unchanged:
43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97.
Original os.html is retained. No new release number is assigned.

## Remaining limitations

- No accelerated GPU driver, driver health checks or automatic hardware-backend
  selection. Read-only inventory is limited to PCI segment 0.
- Normal notices can evict an older sticky warning; notices are not a history UI.
- Saved pin choices are not automatically durable on RAM-only storage.
- Uninstall targets the registered package, not every duplicate package with the
  same ID at other paths. A remaining duplicate may be discovered on next boot.
- Projects/private data are intentionally retained; uninstall is not a data wipe.
- Only existing ATA persistence is tested. Physical USB-boot behavior needs user
  validation; this update does not add storage drivers.
