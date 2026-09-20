# CAT / Studio project format, version 1

All integers are little-endian unsigned 32-bit. Header size is 128 bytes.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `SCOSCAT1` executable package, or `SCOSPRJ1` non-executable project |
| 8 | 4 | Header size, exactly 128 |
| 12 | 4 | Source payload byte length, at most 65536 |
| 16 | 4 | Standard reflected CRC32 (polynomial 0xedb88320) of entire file with these four bytes zeroed |
| 20 | 4 | Required SCos API version, exactly 2 |
| 24 | 4 | Permissions: bit 0 allows requesting global theme changes; all other bits rejected |
| 28 | 4 | Initial outer window width, 320..2048 |
| 32 | 4 | Initial outer window height, 200..2048 |
| 36 | 32 | NUL-terminated ID, 1..30 lowercase ASCII letters/digits/dashes |
| 68 | 40 | NUL-terminated title, 1..39 printable ASCII characters |
| 108 | 20 | Reserved, must be zero |
| 128 | length | Lua source bytes, no NUL; cannot begin with Lua binary escape byte 27 |

File size must exactly match header + payload. CAT requires nonempty source;
projects may be empty or syntactically unfinished. Writers zero metadata padding;
readers require each metadata field's final byte to be zero and validate its
string up to the first NUL. Unknown API, flags, dimensions and reserved fields
are rejected even when the CRC is correct.

`cat_build` invokes the actual Lua compiler without executing the program,
then creates and validates the package before writing it. Failed compilation
leaves an existing package untouched. At launch the loader checks format,
CRC, ID/path ownership and native-ID collisions, snapshots permissions, and
compiles the source into a fresh bounded Lua state. Projects and raw source
are not executable. The file extension alone cannot turn source into a CAT.

These files are transparent source containers, **not signatures, encryption,
native ELF files or exported Lua bytecode**. Someone can modify a package and
recalculate CRC; only run code you trust. Language restrictions do not provide
ring-3 isolation in the cooperative kernel.

Custom themes use `/home/themes/user-<name>.theme`: 48 bytes comprising
`SCOSTH1` plus NUL (8 bytes), then ten LE32 values: main, bg_top, bg_bot,
win_bg, text, title_text, taskbar_bg, mode, spacing, grid. RGB fields must fit
24 bits, mode is 0..3, spacing 8..256. At most eight are loaded. Theme files
have no signature/checksum; fields and size are validated before use.
