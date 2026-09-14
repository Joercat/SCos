/*
 * SCos native - end-to-end scenario tests.
 *
 * Boots build/scos.img in the headless v86 harness and drives the OS with
 * synthetic mouse/keyboard input, exactly like a user would. Each scenario
 * saves a screenshot into build/tests/ and checks liveness (the compositor
 * keeps repainting) plus a few pixel-level assertions.
 *
 *   node tests/run_tests.mjs            run all scenarios
 *   node tests/run_tests.mjs 3,7        run only scenarios 3 and 7
 */
import fs from "node:fs";
import { boot, sleep } from "./harness.mjs";

const OUT = new URL("./../build/tests/", import.meta.url).pathname;
fs.mkdirSync(OUT, { recursive: true });

const only = process.argv[2] ? process.argv[2].split(",").map(Number) : null;
let failures = 0;

/* window geometry helpers (mirror kernel/src/wm.c placement rules) */
const SCREEN_W = 1024, SCREEN_H = 768, TASKBAR_H = 40, TITLEBAR = 26;
function win_pos(n, dw, dh) {
    return {
        x: 40 + (n * 28) % (SCREEN_W - dw - 60 > 0 ? SCREEN_W - dw - 60 : 1),
        y: 24 + (n * 26) % (SCREEN_H - TASKBAR_H - dh - 40 > 0 ? SCREEN_H - TASKBAR_H - dh - 40 : 1),
    };
}
const ICON_POS = (i) => ({ x: 16 + (i % 8) * 88 + 40, y: 16 + Math.floor(i / 8) * 96 + 44 });
const ICON = { files: 0, terminal: 1, notepad: 2, browser: 3, calendar: 4, settings: 5, about: 6, blackjack: 7 };

async function live(state) {
    const grab = () => {
        const fb = state.framebuffer();
        const out = [];
        for (let y = SCREEN_H - 25; y < SCREEN_H - 8; y++)
            for (let x = 900; x < 1020; x++) {
                const i = (y * fb.w + x) * 4;
                out.push(fb.mem[i], fb.mem[i + 1], fb.mem[i + 2]);
            }
        return out;
    };
    const a = grab();
    await sleep(1100);
    const b = grab();
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return true;
    return false;
}
function px(state, x, y) {
    const fb = state.framebuffer();
    const i = (y * fb.w + x) * 4;
    return [fb.mem[i], fb.mem[i + 1], fb.mem[i + 2]];
}
function shot(state, name) { state.save_png(OUT + name + ".ppm"); }

const scenarios = [];
function test(id, name, fn) { scenarios.push({ id, name, fn }); }

/* ---------------------------------------------------------------- 1 ---- */
test(1, "boot to desktop", async (state) => {
    if (!state.serial.includes("boot complete")) throw new Error("boot incomplete: " + state.serial.slice(-200));
    if (!state.serial.includes("wm: entering main loop")) throw new Error("wm did not start");
    if (!(await live(state))) throw new Error("compositor not repainting");
    const c = px(state, 512, 700);            /* wallpaper, above taskbar */
    if (c[0] === 0 && c[1] === 0 && c[2] === 0) throw new Error("wallpaper black");
    shot(state, "01_desktop");
});

/* ---------------------------------------------------------------- 2 ---- */
test(2, "terminal: commands", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("help\n"); await sleep(1600);
    await state.type("calc 2 + 3\n"); await sleep(500);
    await state.type("echo hello scos\n"); await sleep(500);
    await state.type("ping google.com\n"); await sleep(700);
    await state.type("sysinfo\n"); await sleep(1200);
    await state.type("mkdir documents/testdir\n"); await sleep(400);
    await state.type("touch documents/note.txt\n"); await sleep(400);
    await state.type("ls documents\n"); await sleep(600);
    shot(state, "02_terminal_cmds");
    await state.type("rm documents/note.txt\n"); await sleep(400);
    await state.type("alias ll ls\n"); await sleep(300);
    await state.type("ll /\n"); await sleep(500);
    await state.type("history\n"); await sleep(600);
    shot(state, "03_terminal_hist");
    if (!(await live(state))) throw new Error("frozen after terminal commands");
    /* close via title bar X */
    await state.click(w.x + 700 - 13, w.y + 13); await sleep(400);
});

/* ---------------------------------------------------------------- 3 ---- */
test(3, "window manager: drag / resize / min / max / taskbar", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    let w = win_pos(1, 700, 450);
    /* drag by title bar */
    await state.drag(w.x + 200, w.y + 13, 400, 150);
    await sleep(300);
    shot(state, "04_dragged");
    /* resize from bottom-right handle */
    const nx = 200, ny = 137, nw = 700, nh = 450;
    await state.drag(nx + nw - 6, ny + nh - 6, nx + nw + 60, ny + nh + 40);
    await sleep(300);
    shot(state, "05_resized");
    /* minimize */
    const rw = 760, rh = 490;                 /* after resize */
    await state.click(nx + rw - 57, ny + 13); await sleep(400);
    const gone = px(state, nx + 30, ny + 60);
    shot(state, "06_minimized");
    /* restore from taskbar */
    await state.click(40, SCREEN_H - 20); await sleep(400);
    const back = px(state, nx + 30, ny + 60);
    if (gone[0] === back[0] && gone[1] === back[1] && gone[2] === back[2])
        throw new Error("minimize/restore produced identical pixels");
    /* maximize + restore */
    await state.click(nx + rw - 35, ny + 13); await sleep(400);
    shot(state, "07_maximized");
    await state.click(SCREEN_W - 35, 13); await sleep(400);   /* maximize btn while maximized */
    shot(state, "08_restored");
    /* close */
    await state.click(nx + rw - 13, ny + 13); await sleep(400);
    shot(state, "09_closed");
    if (!(await live(state))) throw new Error("frozen after wm ops");
});

/* ---------------------------------------------------------------- 4 ---- */
test(4, "files explorer: navigate, new folder, delete", async (state) => {
    const p = ICON_POS(ICON.files);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 500);
    shot(state, "10_files_root");
    /* open "home" (second row at root: system, home) */
    const row = (i) => ({ x: w.x + 1 + 120, y: w.y + TITLEBAR + 48 + i * 24 + 12 });
    await state.click(row(1).x, row(1).y); await sleep(500);
    shot(state, "11_files_home");
    /* into documents: listing = dirs-first, reverse creation: desktop, downloads, documents */
    await state.click(row(2).x, row(2).y); await sleep(500);
    shot(state, "12_files_documents");
    /* new folder button (4th toolbar button); dialog pre-fills "new folder" */
    await state.click(w.x + 1 + 8 + 3 * 34 + 15, w.y + TITLEBAR + 8 + 13); await sleep(500);
    shot(state, "13_files_newfolder_dialog");
    for (let i = 0; i < 10; i++) await state.key("backspace");
    await state.type("stuff\n"); await sleep(400);          /* Enter = OK */
    shot(state, "14_files_after_mkdir");
    /* right-click the new folder (dirs first: stuff=0, changelog=1, welcome=2) */
    await state.click(row(0).x, row(0).y, "right"); await sleep(400);
    shot(state, "15_files_context");
    await state.click(row(0).x + 60, row(0).y + 3 + 24 + 12); await sleep(400);  /* "Delete" */
    shot(state, "16_files_confirm_dialog");
    await state.key("enter"); await sleep(400);                              /* confirm */
    shot(state, "17_files_after_delete");
    if (!(await live(state))) throw new Error("frozen in files");
    await state.click(w.x + 700 - 13, w.y + 13); await sleep(300);
});

/* ---------------------------------------------------------------- 5 ---- */
test(5, "notepad: edit + save as", async (state) => {
    const p = ICON_POS(ICON.notepad);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 500);
    await state.type("Hello from the native kernel!\nSecond line.\n"); await sleep(500);
    shot(state, "18_notepad_text");
    /* Save As button */
    await state.click(w.x + 1 + 98 + 40, w.y + TITLEBAR + 5 + 12); await sleep(500);
    shot(state, "19_notepad_saveas_dialog");
    await state.type("/home/documents/kernel-note.txt\n"); await sleep(300);
    await state.click(512 - 60, 384 + 40); await sleep(500);             /* OK */
    shot(state, "20_notepad_saved");
    if (!(await live(state))) throw new Error("frozen in notepad");
    await state.click(w.x + 700 - 13, w.y + 13); await sleep(300);
});

/* ---------------------------------------------------------------- 6 ---- */
test(6, "calendar: month navigation", async (state) => {
    const p = ICON_POS(ICON.calendar);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 600, 460);
    shot(state, "21_calendar");
    await state.click(w.x + 1 + 8 + 34 + 15, w.y + TITLEBAR + 7 + 13); await sleep(400);  /* next */
    shot(state, "22_calendar_next");
    await state.click(w.x + 1 + 8 + 34 + 15, w.y + TITLEBAR + 7 + 13); await sleep(400);
    await state.click(w.x + 1 + 8 + 15, w.y + TITLEBAR + 7 + 13); await sleep(400);       /* prev */
    await state.click(w.x + 1 + 8 + 2 * 34 + 30, w.y + TITLEBAR + 7 + 13); await sleep(400); /* today */
    shot(state, "23_calendar_today");
    /* click a day cell */
    await state.click(w.x + 1 + 100, w.y + TITLEBAR + 120); await sleep(400);
    shot(state, "24_calendar_dayclick");
    await state.click(w.x + 600 - 13, w.y + 13); await sleep(300);
});

/* ---------------------------------------------------------------- 7 ---- */
test(7, "settings: theme switch + about", async (state) => {
    const before = px(state, 300, 300);
    const p = ICON_POS(ICON.settings);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 620, 480);
    shot(state, "25_settings");
    /* theme tile 1 = blue-sky: x=16+(TILE_W+12), y=60 ; TILE_W=130 TILE_H=64 */
    await state.click(w.x + 1 + 16 + 142 + 65, w.y + TITLEBAR + 60 + 32); await sleep(600);
    shot(state, "26_settings_bluesky");
    const after = px(state, 300, 300);
    if (before[0] === after[0] && before[1] === after[1] && before[2] === after[2])
        throw new Error("theme switch did not change wallpaper pixels");
    /* back to matrix for later scenarios */
    await state.click(w.x + 1 + 16 + 65, w.y + TITLEBAR + 60 + 32); await sleep(500);
    await state.click(w.x + 620 - 13, w.y + 13); await sleep(300);
    const a = ICON_POS(ICON.about);
    await state.click(a.x, a.y); await sleep(700);
    shot(state, "27_about");
    const w2 = win_pos(2, 520, 460);
    await state.click(w2.x + 520 - 13, w2.y + 13); await sleep(300);
});

/* ---------------------------------------------------------------- 8 ---- */
test(8, "browser stub dialog", async (state) => {
    const p = ICON_POS(ICON.browser);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 640, 420);
    shot(state, "28_browser_stub");
    await state.click(w.x + 640 - 13, w.y + 13); await sleep(400);   /* close */
    shot(state, "29_browser_stub_closed");
});

/* ---------------------------------------------------------------- 9 ---- */
test(9, "easter egg: file.scv spawns error windows", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("touch documents/file.scv\n"); await sleep(400);
    await state.click(w.x + 700 - 13, w.y + 13); await sleep(300);
    await sleep(3000);                                          /* let them spawn */
    shot(state, "30_easter_egg");
    const t = px(state, 300, 200);                              /* an error window covers desktop */
    /* clean the egg file so later runs stay clean: reboot loses RAM fs anyway */
});

/* --------------------------------------------------------------- 10 ---- */
test(10, "shutdown falls back to power-off screen (no ACPI in v86)", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("shutdown\n"); await sleep(3000);
    shot(state, "31_poweroff");
    const c = px(state, 512, 100);
    if (c[0] > 40 || c[1] > 40 || c[2] > 40) throw new Error("power-off screen not black at top");
});

/* --------------------------------------------------------------- 11 ---- */
test(11, "reboot returns to bootloader", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("reboot\n"); await sleep(4000);
    const n = state.serial.split("[s2] stage2 alive").length - 1;
    if (n < 2) throw new Error("no second boot after reboot (s2 count=" + n + ")");
});


/* --------------------------------------------------------------- 12 ---- */
test(12, "ATA persistence: save survives reboot", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    let w = win_pos(1, 700, 450);
    await state.type("touch documents/persist.txt\n"); await sleep(400);
    await state.type("save\n"); await sleep(1500);
    shot(state, "32_saved_to_disk");
    await state.type("reboot\n"); await sleep(6000);          /* second boot */
    state.cursor.x = 512; state.cursor.y = 384;               /* guest cursor resets on boot */
    await state.click(p.x, p.y); await sleep(700);
    w = win_pos(1, 700, 450);
    await state.type("ls documents\n"); await sleep(800);
    shot(state, "33_after_reboot_ls");
    if (!(await live(state))) throw new Error("not alive after reboot");
});

/* --------------------------------------------------------------- 13 ---- */
test(13, "files open-in-notepad + settings reset", async (state) => {
    const p = ICON_POS(ICON.files);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 500);
    const row = (i) => ({ x: w.x + 1 + 120, y: w.y + TITLEBAR + 48 + i * 24 + 12 });
    await state.click(row(1).x, row(1).y); await sleep(500);   /* home */
    await state.click(row(2).x, row(2).y); await sleep(500);   /* documents */
    await state.click(row(1).x, row(1).y); await sleep(700);   /* welcome.txt -> notepad */
    shot(state, "34_notepad_from_files");
    /* close both windows so the desktop icon row is reachable */
    const nw = win_pos(2, 700, 500);
    await state.click(nw.x + 700 - 13, nw.y + 13); await sleep(300);
    await state.click(w.x + 700 - 13, w.y + 13); await sleep(300);
    /* factory reset via settings */
    const sp = ICON_POS(ICON.settings);
    await state.click(sp.x, sp.y); await sleep(700);
    const sw = win_pos(1, 620, 480);
    /* Factory Reset button: ry = 60 + rows*(TILE_H+12) + 16 + 70 ; TILE_H=80 */
    const ry = 60 + 1 * (80 + 12) + 16 + 70;
    await state.click(sw.x + 1 + 16 + 55, sw.y + TITLEBAR + ry + 13); await sleep(500);
    shot(state, "35_reset_confirm");
    await state.key("enter"); await sleep(1000);
    shot(state, "36_factory_reset_screen");
    /* the machine wipes the fs, shows a message, then restarts itself */
    await sleep(9000);
    state.cursor.x = 512; state.cursor.y = 384;               /* guest cursor resets on boot */
    const n = state.serial.split("[s2] stage2 alive").length - 1;
    if (n < 2) throw new Error("factory reset did not restart (s2 count=" + n + ")");
    if (!(await live(state))) throw new Error("not alive after factory-reset reboot");
});

/* --------------------------------------------------------------- 14 ---- */
test(14, "resize stress (heap coalescing)", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    for (let i = 0; i < 10; i++) {
        await state.drag(w.x + 700 - 6 + i * 0, w.y + 450 - 6, w.x + 760, w.y + 490);
        await state.drag(w.x + 760 - 6, w.y + 490 - 6, w.x + 700, w.y + 450);
    }
    await sleep(400);
    shot(state, "37_after_resize_stress");
    if (!(await live(state))) throw new Error("frozen after resize stress");
    await state.type("echo still alive\n"); await sleep(500);
    shot(state, "38_alive_after_stress");
});

/* --------------------------------------------------------------- 15 ---- */
test(15, "blackjack: deal, stand, new round", async (state) => {
    const p = ICON_POS(ICON.blackjack);
    await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 640, 480);
    shot(state, "39_blackjack_deal");
    /* content height = def_h - TITLEBAR; buttons at y = 454-26-40 .. +28 */
    const by = 480 - TITLEBAR - 40;
    /* Stand button = #2: content x 220..316 */
    await state.click(w.x + 1 + 268, w.y + TITLEBAR + by + 14); await sleep(700);
    shot(state, "40_blackjack_stand");
    const c = px(state, w.x + 1 + 320, w.y + TITLEBAR + 250);   /* result banner bg = black */
    if (c[0] > 40 || c[1] > 40 || c[2] > 40) throw new Error("no result banner after stand");
    /* New Round = button #0: content x 12..108 */
    await state.click(w.x + 1 + 60, w.y + TITLEBAR + by + 14); await sleep(600);
    shot(state, "41_blackjack_newround");
    if (!(await live(state))) throw new Error("frozen during blackjack");
    await state.click(w.x + 640 - 13, w.y + 13); await sleep(300);
});

/* ------------------------------------------------------------- runner ---- */
const list = only ? scenarios.filter((s) => only.includes(s.id)) : scenarios;
for (const s of list) {
    let state = null;
    let err = null;
    const onExcept = (e) => { err = err || e; };
    process.on("uncaughtException", onExcept);
    try {
        state = await boot("build/scos.img");
        await sleep(4500);
        await s.fn(state);
    } catch (e) { err = err || e; }
    process.removeListener("uncaughtException", onExcept);
    if (state) state.destroy();
    await sleep(200);
    if (err) { failures++; console.log(`FAIL  ${s.id} ${s.name}: ${err.message}`); }
    else console.log(`PASS  ${s.id} ${s.name}`);
}
console.log(failures ? `${failures} scenario(s) failed` : "ALL SCENARIOS PASSED");
process.exit(failures ? 1 : 0);
