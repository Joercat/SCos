# Open-source components in the SCos Lua app port

These are vendored source, not host binaries. Builds do not download anything.
SCos remains a custom freestanding x64 kernel. No Linux kernel, network stack,
new hardware driver, host libc, or dynamic linker is imported.

## Lua 5.4.9 — MIT

- Upstream: https://github.com/lua/lua
- Tag `v5.4.9`, commit `312b9efaa1061c2c4cad08554dbc1351c3270eef`.
- Retrieved through GitHub's source archive API. The lua.org release archive
  could not be downloaded from this workspace, so this is explicitly the
  official repository tag, **not** a claim to have verified the FTP tarball.
- Core and library headers/source retained in `lua/`; upstream standalone host
  runners, build helpers, tests and manual omitted. License: `lua/LICENSE`, also retained in `lua.h`.
- Includes the actual lexer, parser, bytecode compiler, VM and garbage collector.
  This is not a Lua-like syntax translator or a browser simulation.
- Local changes: `luaconf.h` limits value-stack growth; `lbaselib.c` excludes
  unavailable/unsafe globals and rejects script `__gc` finalizers;
  `lstrlib.c` and `ltablib.c` poll/charge bounded C-library work;
  `lmathlib.c` seeds its non-cryptographic PRNG from the existing CPU clock.
  The adapter in `kernel/lua/port.h` supplies ASCII classification, C-locale
  formatting/numeric conversion, native non-local returns and lower C-call
  depth. OS and loader libraries, the debug library and coroutine library are
  not linked/exposed. See the app guide for the exact supported API.

## musl 1.2.5 support subset — MIT / compatible per-file notices

- Upstream mirror: https://github.com/ifduyue/musl
- Tag `v1.2.5`, commit `0784374d561435f7c787a555aeab8ede699ed298`.
- Selected scalar math functions, their tables/internal header, and decimal/
  hexadecimal floating-point scanner only. No musl OS layer, allocator,
  pthreads, syscalls or stdio runtime is used.
- `musl/COPYRIGHT` and per-file notices are retained. Some math routines carry
  additional compatible notices in their source headers.
- `floatscan.c` is adapted to a string-only `scos_scan` interface in `scan.h`,
  rather than musl's FILE/stdio infrastructure. Classification and errno use
  the foreground-only adapter. Internal trig symbol names are namespaced at
  compile time to avoid conflicting host-header declarations.

## stb_sprintf — MIT option

- Upstream: https://github.com/nothings/stb
- Commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`, `stb_sprintf.h`.
- Unmodified header; used for the bounded C-format operations required by Lua's
  numeric/string library. We select its MIT license option, in `stb/LICENSE`.

`UPSTREAM-SHA256.json` records original upstream bytes for retained files,
including the pre-modification versions of adapted files. Local modifications
are described above and kept in normal Git history. The application image
includes the license texts and math-file notices at `/system/licenses.txt`.

## Build boundary

The existing Linux x86-64 GCC toolchain supplies C type/prototype headers; no
host library is linked. The explicitly selected Lua/support objects are
partially linked with section garbage collection to discard unused host-facing
helpers. Undefined symbols must resolve against SCos or this support subset at
the final freestanding link. The kernel's existing linker retention policy is
unchanged. No new code is reachable from hardware interrupt callbacks.
