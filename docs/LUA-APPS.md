# App Studio, CAT packages and Lua API 2

All **11 preinstalled desktop applications are native C**, including App Studio.
Counter and Sketch are optional Lua CAT demonstrations, not native built-ins. Themes can change their appearance; editable Lua does not
replace system applications. Lua 5.4.9's real compiler and VM remain available
for your own apps, loaded exclusively from **`.cat` packages**. Raw `.lua`
files and `.project` files cannot be launched as applications.

## Build an app inside SCos

1. Open **App Studio** in the launcher or run `appstrt studio` in Terminal.
2. Edit the starter source. The left pane edits ID, title, initial window size
   and the optional system-theme permission. Use a unique lowercase ID.
3. **Check** performs real syntax compilation without running the code.
4. **Ctrl+S / Save** writes `/home/projects/<id>.project`, including metadata
   and unfinished source. **Ctrl+B / Build** syntax-compiles and writes a
   checksummed `/home/apps/<id>.cat`. A failed build leaves an old package intact.
5. **F5 / Run** builds first, then launches the resulting package in a separate
   window. Runtime errors appear there and in Studio's diagnostics; syntax
   checking alone cannot guarantee runtime success. Close old app instances
   before testing new code; existing instances retain their compiled state.
6. Later use `appstrt /home/apps/<id>.cat`, `appstrt <id>`, the launcher, or
   double-click the package in Files. No OS rebuild or reboot is required.

Studio has line numbers, scrolling, lexical syntax colors, metadata caret
editing, one-level source undo/redo (Ctrl+Z/Y), select-all (Ctrl+A), Shift+navigation and mouse-drag range selection, an API
reference (F1), diagnostics, and New/Open/Save/Check/Build/Run/API/Recover
controls. Ctrl+N/O create/open projects. Open accepts `.project` or `.cat`;
opening another document in an already-running dirty Studio asks before
replacing it. Closing dirty Studio writes a **best-effort RAM recovery copy**
at `/home/projects/studio-recovery.project`; Recover loads it. Valid metadata
is preserved; invalid metadata falls back to recovery defaults. This is not
continuous autosave, multi-level undo, a filesystem tree, autocomplete, or a
full Android Studio port. Coloring is a lightweight lexical aid, not a parser.

## Optional demos — install them yourself

In Files open `/home/demos/README.txt` for the walkthrough. Counter and Sketch
ship there as `counter.cat` and `sketch.cat`; they are **not** discovered at boot
or preinstalled in the launcher. Double-click a package, then approve the native
install confirmation. The installer validates the package and compiles its source,
copies it into `/home/apps`, registers it and creates its desktop shortcut.
It does **not** auto-launch the app. Cancel changes nothing. Installation saves
the current filesystem when a supported ATA persistence disk is available;
otherwise the result explicitly says RAM-only. Failed disk saves are reported.
Conflicting IDs and existing destinations are refused without overwriting files.
This same Files workflow handles other CAT packages outside `/home/apps`.
An installed package can later be opened directly or via `appstrt counter`.
Explicit Terminal launch of an outside CAT remains session registration rather
than a durable installation; use Files to install its copy into `/home/apps`.

To inspect or adapt a demo, use Studio's **Open** button and enter
`/home/demos/counter.cat` or `/home/demos/sketch.cat`. Change the ID for a new app.
Files associates `.project` with Studio and ordinary text with Notepad.
Older saved sample `.project` files are left alone, not deleted on upgrade.

## Bounded Studio clipboard

- **Ctrl+C / Ctrl+X / Ctrl+V** copy, cut and paste. Select source with Shift+arrows,
  Shift+Home/End/PageUp/PageDown, mouse drag, or Ctrl+A. Ctrl+Home/End moves to
  the start/end of the document; Shift can extend selection to these positions.
- Clipboard capacity is **16,384 bytes**, stored in a fixed-size buffer. A copy
  or cut larger than this is rejected, preserving the old clipboard and source.
- Source capacity is **65,536 bytes**. Paste, replacement and auto-indented Enter
  are checked before mutation and never silently truncate or partially insert.
  Accepted replacements/cuts/pastes are one undoable source operation.
- Metadata fields support select-all copy/cut and insertion/replacement paste,
  with their own smaller limits (ID 30, title 39, size fields 7 ASCII bytes).
  Multiline/non-printable field paste is rejected. Metadata has no undo history.
- This is a Studio-local, session-memory clipboard, retained across Studio
  windows—not a host/browser clipboard, a Lua API, or an OS-wide Notepad clipboard.
- The editor footer displays line, column, source bytes and clipboard usage.

### Discovery and names

- `/home/apps/*.cat` is scanned at startup and during app/launcher discovery.
- Explicit paths elsewhere can be launched with `appstrt path/to/name.cat`.
- Package metadata supplies the ID (1–30 lowercase ASCII letters/digits/dashes),
  not its filename. Native IDs and same-ID packages at different paths conflict.
- Source limit is **64 KiB**; up to **32 external registrations per boot**,
  within the 64-entry shared registry and 16-window limit. Deleting a package
  does not reclaim its registration until restart; launching it then fails.
- Header, checksum, source and capability metadata are revalidated on launch.
  Capability changes require closing and reopening the app.
- Search and desktop-icon restoration include discovered packages.

See [the exact package format](CAT-FORMAT.md). CAT carries source plus a manifest;
Build really invokes the compiler, but the stored payload is **not native machine
code** or Lua bytecode. CRC detects corruption, **not authenticity**.

## Callback contract

The top-level chunk must return a table. All callbacks are optional and are
looked up directly in that table (not through its metatable).

| Callback | Arguments / behavior |
| --- | --- |
| `open()` | Runs once after compilation and table validation. |
| `paint(width, height)` | Draw the complete content area. Called on initial display, redraw and resize. |
| `mouse(kind, x, y, button, down, wheel, buttons)` | Content-relative coordinates; kind 1=move, 2=button, 3=wheel. Buttons: left=1, right=2, middle=4; `buttons` is the held-button bitmask. Wheel positive=up. Coordinates can be outside the content on some events; hit-test your controls. |
| `key(code, down, shift, ctrl, alt)` | Character/control code and booleans. Enter=10, Backspace=8, Escape=27, Space=32. Navigation: Up=128, Down=129, Left=130, Right=131, Home=132, End=133, PageUp=134, PageDown=135, Delete=136, Insert=137. |
| `tick(seconds)` | At most 10 times/second while WM is running; real uptime, not a fixed simulated clock. No catch-up guarantee. Call `scos.redraw()` only when display state changes. |

Title-bar movement, window controls, resize allocation and app CPU/memory
accounting stay in the existing native WM. Lua receives its content surface,
not the framebuffer address. There is no user close/finalizer callback: closing
or killing the window releases the entire runtime arena without executing
additional app code.

## `scos` API — 55 functions, `scos.version == 2`

Drawing functions are **paint-only**. RGB colors are integers `0xRRGGBB`;
coordinates are -4096..4096, drawing dimensions 0..4096, text ≤1024 bytes.
Invalid arguments stop the app with a diagnostic. Widgets draw only: handle
clicks in `mouse` and call `hit` yourself. There is no hidden widget state.

| Functions / signatures | Result or behavior |
| --- | --- |
| `clear(rgb)`, `rect(x,y,w,h,rgb)` | Fill content or clipped rectangle. |
| `text(x,y,text,rgb)` | Native bitmap text, not a Unicode font engine. |
| `pixel(x,y,rgb)`, `frame(x,y,w,h,rgb)` | Pixel or outlined rectangle. |
| `line(x,y,x2,y2,rgb)` | Clipped line. |
| `circle(x,y,r,rgb)`, `disc(x,y,r,rgb)` | Outline/filled circle; radius 0..1024. |
| `gradient(x,y,w,h,top,bottom)` | Vertical RGB gradient. |
| `icon(id,x,y,rgb)` | Native icon by valid `ICON_*` numeric index. |
| `icon_size()`, `icons()` | Always returns 24,24; name-to-ID table for folder,terminal,notepad,browser,calendar,settings,info,cards,chart,solitaire. |
| `set_icon(id)` | Sets this registered user app's launcher/desktop icon; fixed native size, no scale parameter. Not in paint. Runtime choice: call in `open` to restore on launch. |
| `text_clip(x,y,width,text,rgb)` | Single-line, bounded text with ellipsis; safely draws nothing when width <8. |
| `text_wrap(x,y,width,height,text,rgb)` | Character wrapping at 8×16; returns rows drawn, complete boolean. At most 127 columns per line; no font scaling. |
| `focused()`, `minimize()` | Focus boolean; minimize this window (not in paint). Restore through the Windows picker. |
| `text_width(text)`, `text_scaled(x,y,text,rgb,scale)` | Width in pixels; scaled text with scale 1..4. |
| `rgb(r,g,b)`, `blend(a,b,percent)` | RGB channels 0..255; 0..100 percent of color b. |
| `clamp(value,min,max)`, `hit(x,y,rx,ry,w,h)` | Numeric clamp; half-open rectangular hit test. |
| `button(x,y,w,h,label,active)` | Themed button drawing. |
| `checkbox(x,y,label,checked)`, `progress(x,y,w,h,percent)` | Themed checkbox/progress drawing, percent 0..100. |
| `size()`, `screen()` | Content width,height; screen width,height. |
| `window()` | Table: x,y,width,height (outer geometry), state,focused. |
| `resize(width,height)`, `move(x,y)` | Success boolean; not during paint. Resize 320..2048 × 200..2048; screen/app-min clamped. Refused during drag/resize or non-normal window state. Allocation failure leaves geometry intact. |
| `mouse()` | Last content-relative x,y,held-button bitmask (buttons zero when unfocused). |
| `redraw()`, `title(text)` | Request deferred repaint; set title (≤63 bytes). |
| `interval(ms)` | Tick interval 100..60000 ms, rounded to timer granularity. |
| `time()`, `uptime()` | Real PIT uptime in seconds; aliases. |
| `date()` | RTC table: year,month,day,hour,minute,second,weekday. |
| `app_id()`, `memory()` | Metadata ID; owner bytes and Lua arena limit (not remaining free bytes). |
| `log(text)` | Launching terminal log, ≤512 bytes. |
| `read(name)`, `write(name,contents)` | String/nil; success boolean. |
| `exists(name)`, `file_size(name)` | Boolean; byte count/nil. |
| `remove(name)`, `rename(old,new)`, `files()` | Success booleans; array of private filenames. |
| `theme()`, `themes()` | Current palette table; array of available IDs. |
| `theme_apply(id)`, `theme_custom(id,fields)` | Request native confirmation, return whether prompt opened; permission required. |
| `theme_status()` | Global latest request: idle/pending/applied/denied/failed, not per-app ownership. |
| `message(text)` | Native message dialog, ≤192 bytes; boolean indicating whether shown. Not in paint. |
| `api_info()` | Table: version,source_limit,arena_limit,data_limit,data_files,theme_permission. |

Private files live in `/home/appdata/<id>/`. Names allow ASCII letters/digits,
`.`, `_`, `-`, but not `.`/`..` or slashes. Up to 16 files/64 KiB per ID through
this API. Instances share that directory but have separate Lua states. There
is no general filesystem, shell, network or device access.

## Create and select system themes

Settings selects four presets plus **eight custom slots**; use its mouse wheel
in shorter windows to reach lower controls. Themes affect desktop backgrounds,
window chrome, text, taskbar, menus, dialogs and native app palettes. Semantic
alert colors and playing-card colors remain purposeful exceptions. User apps
should use `scos.theme()` rather than hardcoded colors to follow appearance.

In Studio enable **theme permission**, replace the source with this, and Run:

```lua
assert(scos.theme_custom("user-midnight", {
    main = 0x64b5f6, text = 0xe6edf3, win_bg = 0x18202c,
    title_text = 0xffffff, taskbar_bg = 0x101722,
    bg_top = 0x101722, bg_bot = 0x243c58,
    mode = 2, spacing = 32, grid = 0x294059
}))
return {
    paint = function(w, h)
        local t = scos.theme()
        scos.clear(t.win_bg)
        scos.text(16, 20, "Theme request: " .. scos.theme_status(), t.text)
    end,
    tick = function() scos.redraw() end
}
```

Approve the **native system confirmation** to apply it. The ID must start with
`user-`, contain lowercase letters/digits/dashes, and total at most 30 bytes.
Unspecified fields inherit the current theme. Modes: 0 solid, 1 gradient,
2 grid, 3 stripes; spacing 8..256 pixels. Other fields are RGB values.
Existing custom IDs can be overwritten; a ninth new ID fails without replacing
another slot. Selection can also be requested with `theme_apply(id)`.
Requests cannot originate in paint, are throttled to once per five seconds per
instance, and fail to open while another modal is active. A returned `true`
means **prompt opened**, not user approval or successful persistence.
Message dialogs have a separate five-second throttle.

## Saving and persistence

Studio, custom themes and app-data writes save to the **RAM VFS**.
Explicit package installation additionally attempts a whole-VFS save on supported
ATA storage, after the install confirmation explains this. Recovery is
also RAM-only until persisted. On an existing supported/verified ATA target,
the Terminal's confirmed `save` command persists the tree. The saved region is
128 KiB for the whole filesystem, including licenses and other files. Per-app
quotas are ceilings, not guarantees that a combined disk save will fit.

USB boot/AHCI/NVMe still lack supported native persistence: your changes are
lost on reboot there. No new storage drivers were added. Missing demo
packages are supplied on upgrade without overwriting existing edits;
old `.lua` files remain ordinary data, never executable apps.

## Lua compatibility and limits

Available: ordinary Lua language features, closures, tables, normal metatables,
integer/float arithmetic and the `math`, `string`, `table`, `utf8` libraries.
Numeric/string formatting and math use the small support subsets documented in
[third_party/README.md](../third_party/README.md). `math.random` is a PRNG, not
cryptographic randomness.

Deliberately unavailable: `io`, `os`, `package`, `require`, debug access,
coroutines, `load`, `loadfile`, `dofile`, `string.dump`, `pcall`, `xpcall`,
`collectgarbage`, `print`, `warn`, native shared modules and binary chunks.
Use `scos.log` for output. `pcall`/`xpcall` are excluded so a script cannot catch
and ignore its execution-limit exception. Script GC finalizers (`__gc`) cannot
be registered: upstream disables debug hooks while executing those callbacks.
The runtime still performs automatic garbage collection normally.

Each window gets a **2 MiB Lua arena**, released on close, in addition to native
window/state storage. C-call recursion is limited to 48; the Lua value stack is
limited to 15,000 slots. Each callback has a 100,000-instruction/work budget, an
existing-timer deadline of 100 ms, and at most four million charged drawing
pixels. Pattern matching, plain string search and potentially large table loops
also charge/check C-library work; expensive calls can fail even without a long
Lua loop. These are conservative responsiveness limits, not hard real-time
scheduling guarantees or a proof that arbitrary code can never expose a bug.

**This is not ring-3 isolation.** Apps still run inside the cooperative kernel.
The Lua API limits language-level access and catches tested failures; an
implementation bug in the interpreter/adapter can still affect the OS. Do not
use the first port as a security boundary for hostile downloaded code.

## Native C app registration

Native apps still build into the kernel, but the compositor no longer owns a
list of their names/icons/labels. Each native app declares its existing
`struct app` callbacks and adds, in its own source file:

```c
SCOS_APP(app_my_app, 100);
```

The linker retains and orders these registrations. `apps.c` validates IDs and
provides shared lookup/registration. Optional `desktop_label` is short icon
metadata; `file_editor` identifies the fallback document editor; `file_suffix` and
`document` support native document associations and single-instance opening. The WM handles generic registered clients; its internal dialogs
remain native WM UI. Adding a C app requires a build; adding a Lua app does not.
There is no native ELF executable loader in this change.
