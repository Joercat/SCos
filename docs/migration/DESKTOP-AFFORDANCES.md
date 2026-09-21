# Desktop affordances: which surface owns which action

Date: 2026-09-21. Status: implemented and verified in the guest suites.

This document records a decision about *where* commands live rather than about a widget's pixels: SCos
has no application-manager window. Everything that window used to gather — launch, pin, hide, restore,
uninstall, install — now belongs to the surface the action is performed on. The reason is not taste. A
hub lists every application and offers every command to every selection, so it can only answer "what can
I do in general", and the answer to that question is a wall of buttons that are usually meaningless:
"Uninstall" on a built-in, "Pin" on something already pinned, "Hide" on a selection of three files.
Per-surface entry points can be computed from the thing under the pointer, and when they are, a menu
that has three lines means there are exactly three things to do here.

## 1. The tables, and where each line is computed

| Surface | Raised by | Actions |
| --- | --- | --- |
| A desktop application icon | right-click | Open · Pin to taskbar / Unpin from taskbar · Hide this icon · Uninstall app (only for a `.cat` app) |
| A desktop file shortcut | right-click | Open · Show folder in Files · Remove this shortcut |
| A rubber-band selection | right-click | Remove from Desktop · Clear Selection |
| Empty wallpaper | right-click | Restore hidden icons · Open Files · Open Terminal |
| A taskbar entry | right-click | Open · Unpin from bar · Put its icon back (only when that icon is hidden) |
| A row in Files (a file) | right-click | Open · Open with... · Rename... · Pin to Desktop · Delete |
| A row in Files (a directory) | right-click | Open · Rename... · Pin to Desktop · Delete |
| A synthesised Files row (app shortcut, pinned path) | right-click | Open |

Two rules hold throughout. A line is only offered where it can be carried out: a built-in is never
offered "Uninstall", a shortcut is never offered "Rename" (it is not a file in that folder), and "Put
its icon back" appears on the bar only for an app whose icon was actually put away. And the *protection*
is never the menu: `app_request_uninstall` refuses built-ins on its own, Files prints "Delete files with
`rm -s` in the terminal" instead of unlinking anything, and hiding an icon is reversible from both the
wallpaper menu and the taskbar.

The icon table is one function — `wm_desk_actions()` — and `wm_desk_invoke()` runs the row chosen from
it. That pairing exists so that the menu, any keyboard path, and the regression suite cannot end up
describing the desktop three different ways; there is no second list of labels to keep in step.

## 2. Drag and drop, and what a drop means

A drag carries one of two payloads, because those are the two things a desktop can hold: an application
id, or a path. `wm_dnd_begin()` starts one, the WM paints a label that follows the pointer, and
`wm_dnd_drop()` resolves the release from what is underneath:

| released over | with an application | with a path |
| --- | --- | --- |
| a window | raise that window | open the file *in that application* (`app->document` if it already runs) |
| a taskbar entry | pin it (refused, with the width rule explained, if the bar is full) | open the file with that application |
| the wallpaper | install the application's icon | leave a shortcut to the file |
| a menu, the launcher, or nothing | the drag is cancelled | the drag is cancelled |

Sources: a row of Files (past six pixels of movement, the same threshold the icon grid uses), and a row
of the launcher. Nothing reorders the launcher's behaviour more than necessary — the press is remembered
and the *release* decides, so a click on a row still launches the app exactly as before, and the drop
target is visible while dragging because the launcher closes the moment a drag begins.

Files is the one application whose content is a folder, so a directory dropped on it navigates instead of
being treated as a document. A drop that lands nowhere is a cancellation, not a half-executed action:
the payload is copied out and the drag state is cleared before any target runs, so a target that opens a
window and repaints never observes a drag still in progress.

`wm_dnd_target(px, py, out, cap)` answers "what would this do?" without doing it. It is part of the
design rather than a test hook: every rule in the table above is decided by the pointer, so a claim about
those rules has to be checkable on a chosen geometry — and the suite does exactly that, per row, before it
asserts that the drop changed what it said it would change.

## 3. What was removed, and what replaced each part

`kernel/desktop/app_applications.c` (the `applications` / "Apps" window: a list of every app with
Launch, Pin / Unpin and Uninstall buttons) is deleted, along with its default taskbar pin — the bar now
starts with files, terminal, studio and settings. Nothing else had to be added to keep its functions:

* launch — desktop icon, launcher, taskbar entry, `appstrt <id>` in Terminal;
* pin / unpin — the icon's menu, the bar's menu, dragging an app onto the bar;
* hide / restore — the icon's menu, the wallpaper's menu, the bar's menu;
* uninstall — the icon's menu (external apps), `appuninstall <id>`, both through the one reusable
  default-no confirmation;
* install / shortcut — dragging from the launcher, `Pin to Desktop` in Files, `appstrt` of a `.cat` path;
* "what is installed" — the launcher (searchable, including hidden entries), `apps` in Terminal, and
  System Monitor for per-application memory and CPU.

## 4. Verification

`tools/tests/test_compositor.py` (1 and 2 vCPU) reads an icon's action table out of the WM, right-clicks
that icon with synthesised events, checks the raised menu is *the same table*, chooses "Hide this icon",
and proves the pixels where the icon was differ from before and then that a damaged repaint there equals
a full rebuild; it restores the icon and repeats. It drags a path onto the wallpaper, onto a Notepad
window and onto a taskbar entry, asserting `wm_dnd_target()` first and the resulting state after, and
checks the shadow's damage on three moves (the label clamps to the far side of the cursor near an edge,
so the painter records the box it covered instead of the test guessing it). It also verifies the launcher:
a click still launches, and press-and-move installs.

`tools/tests/test_management.py` renames a file through Files' row menu (dialog, typed name, old name
gone, new name on disk, the list follows the rename), hands that file to Notepad through `Open with...`,
asserts the refused pin beyond the bar's measured capacity and that the same application pins once a slot
is freed, and uninstalls a `.cat` application from its own desktop icon: window closed, package removed,
icon and pin gone — and after a reboot on a writable disk, `system/desktop.json` still says so.

Both suites drive the shipped `handle_mouse`, `wm_desk_*` and `wm_dnd_*`, not copies of their logic.
