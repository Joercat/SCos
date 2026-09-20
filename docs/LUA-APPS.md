# Write an SCos app in Lua

SCos embeds the **Lua 5.4.9 lexer, parser, bytecode compiler and VM**. A `.lua`
file is compiled in memory when it launches; no kernel rebuild or reboot is
needed. This is real native execution, not the old web simulation.

## Try the included apps

In the SCos Terminal:

```text
appstrt counter
appstrt sketch
```

- **Counter:** click its button or press Space. Press S to write its count into
  `/home/appdata/counter/count.txt` in the RAM filesystem.
- **Sketch:** click to draw, resize the window, press R to clear.

Their editable sources are `/home/apps/counter.lua` and `/home/apps/sketch.lua`.
The repository originals are in `apps/examples/`. The examples are initial VFS
resources, not special cases in the compositor or interpreter.

## Create your own app

```text
touch /home/apps/hello.lua
appstrt notepad /home/apps/hello.lua
```

Enter this source and save it with Ctrl+S:

```lua
local clicks = 0
return {
    open = function()
        scos.title("Hello from my app")
    end,
    paint = function(width, height)
        scos.clear(0x101820)
        scos.text(16, 16, "Clicks: " .. clicks, 0x39ff14)
        scos.rect(16, 48, math.max(0, width - 32), 32, 0x17402a)
    end,
    mouse = function(kind, x, y, button, down, wheel, buttons)
        if kind == 2 and button == 1 and down then
            clicks = clicks + 1
            scos.redraw()
        end
    end
}
```

Then run:

```text
appstrt /home/apps/hello.lua
```

After discovery, `appstrt hello` also works. Relative file paths use the
terminal's current directory. After editing, **close and relaunch** to compile
the new source. Existing windows retain their own compiled code and state.
Syntax/runtime failures leave an error window that can be closed normally; a
launching terminal receives the error log too. Launching a file never executes
it at discovery time.

### Discovery and names

- `/home/apps/*.lua` is scanned at startup, on `apps`/`appstrt`, when opening
  the launcher, and when restoring desktop icons.
- Other VFS paths can be launched explicitly with `appstrt path/to/name.lua`.
- The filename without `.lua` is the app ID: 1–30 characters, lowercase ASCII
  letters, digits or `-`. IDs must be unique and cannot replace native apps.
- Source limit: **64 KiB**. Up to **32 Lua app IDs per boot session**, with a
  64-entry shared registry. The existing 16-window limit still applies.
- Registry entries live for the session; deleting a source makes subsequent
  launches report a missing-source error. Restart clears transient entries.
- Search includes Lua apps. Scroll the launcher to reach results beyond its
  ten visible rows. “Restore removed icons” adds discovered apps while desktop
  grid capacity permits. Internal dialog/error clients are not listed.

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

## `scos` API, version 1

`scos.version` is the integer `1`.

| Function | Meaning |
| --- | --- |
| `scos.clear(rgb)` | Fill this window's content. |
| `scos.rect(x,y,w,h,rgb)` | Filled rectangle, clipped to this window. |
| `scos.text(x,y,text,rgb)` | Native bitmap text (UTF-8 string operations exist, but the native font renderer is not a Unicode font engine). |
| `scos.size()` | Return current content width, height. |
| `scos.redraw()` | Request a paint; does not recursively invoke the app. |
| `scos.title(text)` | Change this window's title, maximum 63 bytes. |
| `scos.log(text)` | Send a message to the app's launching terminal, maximum 512 bytes. |
| `scos.time()` | Real uptime in seconds from the existing PIT clock. |
| `scos.read(name)` | Read a file under `/home/appdata/<app-id>/`; nil if absent. |
| `scos.write(name, contents)` | Write there in RAM; returns success boolean. |

Drawing is allowed **only inside `paint`**. Colors are integer `0xRRGGBB`.
Coordinates range from -4096 to 4096; dimensions from 0 to 4096; individual text
calls accept up to 1024 bytes. Out-of-range arguments stop the app instead of
being passed unchecked to native drawing code.

Data filenames allow ASCII letters, digits, `.`, `_`, `-`, but not `.`/`..`,
slashes or path traversal. Each app ID has at most 16 files / 64 KiB total through
this API. Instances have **separate Lua states**, but deliberately share the
same app-ID data directory. There is no general filesystem, device, network or
shell-command API in Lua.

## Saving and persistence

Notepad save and `scos.write` update the existing **RAM VFS**. They do not promise
durable disk writes. On the existing supported/verified ATA target, use the
terminal's confirmed `save` command to persist the tree. The saved partition
remains only 128 KiB for the whole filesystem, including other files and license
notices: app-local quotas are ceilings, not a promise that every combination
will fit a disk save. The OS must report a failed/oversized save rather than
pretend success.

USB-storage boot still lacks native USB disk persistence. On that configuration,
custom sources and data are lost on shutdown/reboot. No storage driver was added
for this feature. Do not reset the filesystem merely to obtain examples: an
older saved tree without `/home/apps` gets the examples on upgrade; existing
app directories and edited sources are not replaced.

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
metadata; `file_editor` identifies the document editor without naming it in the
compositor. The WM handles generic registered clients; its internal dialogs
remain native WM UI. Adding a C app requires a build; adding a Lua app does not.
There is no native ELF executable loader in this change.
