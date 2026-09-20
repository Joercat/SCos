# Frozen i386 implementation / porting reference

These are the previous SCos bootloader, kernel, desktop and driver sources,
relocated when the user authorized AMD64 conversion. They are not part of the
active root build. Keep the physically validated HID behavior and desktop
semantics as reference for subsequent ports.

From the repository root, `make legacy` builds `legacy/i386/build/scos.img`.
This rebuild was verified byte-identical to frozen `dist/scos-32bit.img` (r43).
Only relative shared-tool paths in this Makefile changed during relocation.
Do not overwrite the frozen artifact. `os.html` remains at the repository root.

The active unnumbered AMD64 foundation is documented in
`../../docs/migration/BOOT64.md`. Neither implementation is a Linux distribution.
