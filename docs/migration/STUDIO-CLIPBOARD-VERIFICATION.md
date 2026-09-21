# Optional CAT demos and bounded Studio editing verification

Historical baseline. Installer auto-launch behavior is superseded by
[the compositor/installer verification](COMPOSITOR-VERIFICATION.md).

Unnumbered x64 development build, 2026-09-20 local session date.
This supersedes the demo distribution/editor portions of CAT-VERIFICATION.md.
No new drivers, no physical-PC acceptance, no frozen-32-bit changes.

## Changes

- Removed native Counter/Sketch implementations and registrations. A fresh boot
  has eleven visible native applications (plus two internal WM clients). A later change removed the
  eleventh (the `Applications` manager) in favour of per-icon and per-surface affordances, so a fresh
  boot now has ten.
- Optional Lua demonstrations are packaged under `/home/demos/*.cat`, outside
  boot discovery. An in-OS README explains installing them and opening their
  source in Studio. They are not in the launcher before explicit registration.
- Files prompts before installing outside CAT files into `/home/apps`, validates
  format and syntax, rejects existing destinations/IDs, and rolls back the new
  copy if registration fails. Cancel performs no write or registration.
- Studio supports range selection (Shift+navigation and mouse drag), Ctrl+C/X/V,
  Ctrl+Home/End, selection highlighting, and line/column/byte-count status.
- Clipboard is a fixed 16,385-byte buffer holding at most 16,384 text bytes plus
  terminator. Source is limited to 65,536 bytes. Oversize copy/cut preserves both
  old clipboard and source; oversize paste preserves source and undo history.
- Source insertion and auto-indent are checked and committed as one operation.
  This fixes partial auto-indent near the source limit. Non-Shift navigation
  clears selection consistently (previous Home/End/vertical movement did not).
  Selected metadata Delete now clears and marks the edit dirty.

## Actual checks

Two clean builds (`make clean && make -j4`) produced byte-identical images,
with no compiler warnings/errors. Image is 67,108,864 bytes, SHA256:

```
2f00489d092f734a561ad08e184dd1c039c66a173b5209ca4204ed2408240f88
```

Both retained suites passed on q35 USB/xHCI and compatible IDE/PS2 guests:

```
python3 tools/tests/test_cat.py
python3 tools/tests/test_studio_edit.py
```

CAT coverage includes the existing malformed metadata/compiler/runtime/theme/
Studio checks, eleven native app lifecycles, and for both demonstrations:
absence before installation, cancelled installation with no output file,
approved byte-exact installation and real Lua execution. IDE additionally
performs actual VFS save, machine reboot and package/theme rediscovery.

Editor coverage uses actual QMP key events and production mouse callbacks:
Shift+arrow selection; copy, cut, paste and undo/redo; navigation clearing
selection; exact 16 KiB copy/cut/paste; rejection at 16 KiB+1 preserving the
clipboard and source; selection replacement with undo; rejection of paste and
Enter into a full 64 KiB source; metadata overflow refusal; mouse-drag range
cut/paste. A failing early run exposed a harness timing race: a 60 ms wait could
pause before queued USB events were handled. The retained key-delivery wait is
now 200 ms; final suites passed. This was not treated as a successful run.

## Limits

Clipboard is Studio-local session RAM, not host/browser clipboard integration
or cross-application clipboard support. Source undo/redo remains one level;
metadata has no undo. Dragging selects within visible editor rows, without
auto-scrolling beyond the viewport. No search/tree/autocomplete added.

USB boot still has no native disk persistence. Installer writes are RAM-only
until a supported ATA save; no promise of persistent USB installation. Tests
exercise native production callbacks and emulator input, not physical hardware
or every possible malformed package/editor event sequence. Ring-0 cooperative
Lua and CRC packages are not a security boundary for hostile applications.
