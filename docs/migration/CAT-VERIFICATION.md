# CAT / native Studio / API 2 verification

Date: 2026-09-20. Unnumbered x64 development build; **not physical-PC acceptance**.
No new drivers were introduced. `os.html` and the permanently frozen 32-bit
milestone remain in place. The older LUA-VERIFICATION.md records the previous
raw-source interface and is preserved as history, not current instructions.

## Reproducible image

Two complete `make clean && make -j4` builds produced byte-identical 67,108,864-byte
UEFI images, with no compiler warnings/errors:

```
b7625710bd571d3d136877f87d2286fd92bbe0f0c30d1266e1a01a5952ee807f
```

`python3 tools/check_milestone.py` passed, retaining frozen image SHA256:
`43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97`.

## Retained actual-guest tests

Run from repository root after `make` and `python3 tools/setup_qemu.py`:

```sh
python3 tools/tests/test_cat.py
```

Both 128 MiB guests passed: q35 USB boot/xHCI keyboard, and i440fx compatible
IDE/PS2. Tests use the actual kernel, Lua compiler/VM, native VFS, compositor,
QMP keyboard input and idle-boundary GDB calls into production functions.
There are no guest test hooks or fabricated success screens. GDB injection
is for deterministic API assertions, not a claim that every path was exercised
through physical mouse/keyboard input. Disposable image copies protect the
published image and host disks. Logs and screenshots stay under ignored
`build/test-results/`; the Python harness is retained for reuse.

Verified in both guests:

- Raw `.lua` launch rejection; truncated/extended/corrupt CAT rejection;
  malformed but rechecksummed API/flags/dimensions/reserved fields;
  native application ID collision rejection.
- Genuine syntax diagnostics, independent Python/native byte-exact CAT
  comparison, preservation of an old package after failed compilation.
- Drawing/widget/query/private-data APIs, successful resize/move, forbidden
  paint-time resize, invalid dimensions, permission denial, paint/instruction/
  memory/path failures, owner allocations released after tested app closes.
- Native approval of custom colors/backgrounds, cancellation without writing,
  all eight custom slots, ninth-slot failure without overwriting another theme.
- All thirteen native C app lifecycles under a custom palette. Settings in a
  560×300 window scrolls down to its lower controls with twelve themes loaded.
- Studio Ctrl+S and F5 actually save/build/run; bundled project opening;
  single-instance document routing; source undo/redo; cancellation of dirty
  document replacement; close-time recovery of source and valid metadata;
  direct CAT document opening launches the package.
- On IDE, actual `fs_image_save`, machine reboot, custom-theme restoration,
  restored project/package and package rediscovery/execution.

The final USB Studio and short-Settings screenshots were inspected. The test
palette deliberately uses a dark accent, demonstrating that custom themes can
have poor contrast; this implementation does not enforce accessibility contrast.

## Scope and remaining limitations

All shipped desktop applications are C. Bundled editable sample projects are
user-app templates, not the implementations of Counter/Sketch. CAT contains
validated source/manifest, not native executables or signatures. API 2 has 48
functions; see ../LUA-APPS.md and ../CAT-FORMAT.md for contracts and limits.

Studio is a single-file project workbench, not a full Android Studio clone:
one-level undo/redo, lightweight lexical coloring, no autocomplete/tree/search,
and best-effort recovery on dirty close rather than continuous autosave.
Metadata editing is implemented but not exhaustively keyboard-regression-tested.
Resize allocation is transactional by inspection; allocation failure during
resize was not fault-injected. Smaller display modes were not boot-tested;
the shorter-window test above ran at the normal 1024×768 guest resolution.

USB/AHCI/NVMe persistence is still unavailable; RAM changes disappear on reboot.
Supported IDE persistence is limited to 128 KiB for the whole VFS. Scripts run
cooperatively in ring 0: limits contain tested failures, not arbitrary hostile
native bugs. This suite does not certify every original application control,
every Lua function argument combination, hardware input, or power-loss safety.
