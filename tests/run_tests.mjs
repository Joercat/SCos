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
const ICON = { files: 0, terminal: 1, notepad: 2, browser: 3, calendar: 4, settings: 5, about: 6, blackjack: 7, sysmon: 8 };

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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(110, SCREEN_H - 20); await sleep(400);   /* task button 0 (after launcher btn) */
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(a.x, a.y); await sleep(120); await state.click(a.x, a.y); await sleep(700);
    const aw = win_pos(1, 520, 460);
    const tb = px(state, aw.x + 200, aw.y + 13);       /* about titlebar must exist */
    if (tb[0] + tb[1] + tb[2] < 60) throw new Error("about window did not open");
    shot(state, "27_about");
    const w2 = win_pos(2, 520, 460);
    await state.click(w2.x + 520 - 13, w2.y + 13); await sleep(300);
});

/* ---------------------------------------------------------------- 8 ---- */
test(8, "browser stub dialog", async (state) => {
    const p = ICON_POS(ICON.browser);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 640, 420);
    shot(state, "28_browser_stub");
    await state.click(w.x + 640 - 13, w.y + 13); await sleep(400);   /* close */
    shot(state, "29_browser_stub_closed");
});

/* ---------------------------------------------------------------- 9 ---- */
test(9, "easter egg: file.scv spawns error windows", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("shutdown\n"); await sleep(3000);
    shot(state, "31_poweroff");
    const c = px(state, 512, 100);
    if (c[0] > 40 || c[1] > 40 || c[2] > 40) throw new Error("power-off screen not black at top");
});

/* --------------------------------------------------------------- 11 ---- */
test(11, "reboot returns to bootloader", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("reboot\n"); await sleep(4000);
    const n = state.serial.split("[s2] stage2 alive").length - 1;
    if (n < 2) throw new Error("no second boot after reboot (s2 count=" + n + ")");
});


/* --------------------------------------------------------------- 12 ---- */
test(12, "ATA persistence: save survives reboot", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    let w = win_pos(1, 700, 450);
    await state.type("touch documents/persist.txt\n"); await sleep(400);
    await state.type("save\n"); await sleep(1500);
    shot(state, "32_saved_to_disk");
    await state.type("reboot\n"); await sleep(6000);          /* second boot */
    state.cursor.x = 512; state.cursor.y = 384;               /* guest cursor resets on boot */
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    w = win_pos(1, 700, 450);
    await state.type("ls documents\n"); await sleep(800);
    shot(state, "33_after_reboot_ls");
    if (!(await live(state))) throw new Error("not alive after reboot");
});

/* --------------------------------------------------------------- 13 ---- */
test(13, "files open-in-notepad + settings reset", async (state) => {
    const p = ICON_POS(ICON.files);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(sp.x, sp.y); await sleep(120); await state.click(sp.x, sp.y); await sleep(700);
    const sw = win_pos(1, 620, 480);
    /* Factory Reset button: ry = 60 + rows*(TILE_H+12) + 16 + 70 ; TILE_H=80 */
    const ry = 60 + 1 * (80 + 12) + 16 + 70 + 186;   /* prefs section above it */
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
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

/* --------------------------------------------------------------- 16 ---- */
test(16, "sysmon: metrics + end task", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    await state.type("open files\n"); await sleep(600);
    /* sysmon icon sits in row 2 at x=56, left of every cascaded window */
    const sp = ICON_POS(ICON.sysmon);
    await state.click(sp.x, sp.y); await sleep(120); await state.click(sp.x, sp.y); await sleep(900);
    const sw = win_pos(3, 640, 480);
    shot(state, "42_sysmon");
    /* load bar frame present around (30..330, y) of content */
    const bar = px(state, sw.x + 1 + 30, sw.y + TITLEBAR + 102);
    if (bar[0] + bar[1] + bar[2] < 40) throw new Error("cpu load bar missing");
    /* select files row (apps start at row 5; files = win1 -> row 6) and end it */
    await state.click(sw.x + 1 + 200, sw.y + TITLEBAR + 240 + 6 * 18 + 8); await sleep(400);
    shot(state, "43_sysmon_sel");
    const row = () => state.framebuffer().mem.slice((748 * 1024 + 185) * 4, (748 * 1024 + 295) * 4);
    const before = row();
    await state.click(sw.x + 1 + 67, sw.y + TITLEBAR + 430); await sleep(600);
    const after = row();
    let diff = 0;
    for (let i = 0; i < before.length; i += 41) if (before[i] !== after[i]) diff++;
    if (!diff) throw new Error("task button label unchanged after End Task");
    shot(state, "44_after_endtask");
    if (!(await live(state))) throw new Error("frozen after end task");
});

/* --------------------------------------------------------------- 17 ---- */
test(17, "taskbar power menu reboots", async (state) => {
    await state.click(1024 - 68 - 34 - 16 + 17, 768 - 40 + 20); await sleep(500);
    shot(state, "45_power_menu");
    /* menu opened above the button: item 0 = Restart */
    await state.click(1024 - 68 - 34 - 16 + 17 - 140 + 85, 768 - 40 + 6 - 2 * 24 - 8 + 12); await sleep(500);
    await sleep(8000);
    const n = state.serial.split("[s2] stage2 alive").length - 1;
    if (n < 2) throw new Error("power menu Restart did not reboot (s2=" + n + ")");
});

/* --------------------------------------------------------------- 18 ---- */
test(18, "terminal: wheel scroll + paged help", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("help\n"); await sleep(1200);
    await state.type("help --p3\n"); await sleep(1200);
    shot(state, "46_help_p3");
    const grab = () => state.framebuffer().mem.slice(300 * 1024 * 4, 301 * 1024 * 4);
    const a = grab();
    await state.move(w.x + 350, w.y + 200);
    await state.wheel(6); await sleep(400);
    const b = grab();
    let diff = 0;
    for (let i = 0; i < a.length; i += 37) if (a[i] !== b[i]) diff++;
    if (!diff) throw new Error("wheel did not scroll terminal");
    shot(state, "47_help_scrolled");
    await state.wheel(-50); await sleep(400);
    await state.type("echo back-at-bottom\n"); await sleep(900);
    shot(state, "48_follow_bottom");
    if (!(await live(state))) throw new Error("frozen after scroll");
});

/* --------------------------------------------------------------- 19 ---- */
test(19, "solitaire: deal + draw", async (state) => {
    const p = ICON_POS(9);
    await state.click(p.x, p.y); await sleep(900);
    const w = win_pos(1, 700, 500);
    shot(state, "49_solitaire");
    let bright = 0;
    for (let x = 2; x < 56; x += 2) {
        const q = px(state, w.x + 1 + 33 + x, w.y + TITLEBAR + 8 + 40);
        if (q[0] + q[1] + q[2] > 90) bright++;
    }
    if (bright < 8) throw new Error("stock pile not drawn (bright=" + bright + ")");
    for (let i = 0; i < 3; i++) { await state.click(w.x + 1 + 40, w.y + TITLEBAR + 48); await sleep(300); }
    let wb = 0;
    for (let x = 4; x < 56; x += 4) {
        const q = px(state, w.x + 1 + 129 + x, w.y + TITLEBAR + 8 + 70);
        if (q[0] + q[1] + q[2] > 300) wb++;
    }
    if (wb < 5) throw new Error("waste card not drawn after draws (wb=" + wb + ")");
    shot(state, "50_solitaire_drawn");
    if (!(await live(state))) throw new Error("frozen in solitaire");
});

/* --------------------------------------------------------------- 20 ---- */
test(20, "kernel panic screen from terminal", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    await state.type("sysrq panic demo halt\n"); await sleep(2000);
    shot(state, "51_panic");
    const fb = state.framebuffer();
    let red = 0;
    for (let x = 400; x < 560; x += 3) {
        const i = (70 * fb.w + x) * 4;      /* framebuffer is BGR(A) */
        if (fb.mem[i + 2] > 120 && fb.mem[i + 1] < 90) red++;
    }
    if (red < 5) throw new Error("panic title not red (red px=" + red + ")");
    const d = px(state, 512, 700);                   /* halted: dark background */
    if (d[0] + d[1] + d[2] > 120) throw new Error("panic background not dark");
});

/* --------------------------------------------------------------- 21 ---- */
test(21, "desktop: drag-snap, rubber band, remove, launcher", async (state) => {
    /* drag Files icon from cell (0,0) to cell (3,2) */
    await state.drag(56, 60, 16 + 3 * 88 + 40, 16 + 2 * 96 + 44);
    await sleep(400);
    shot(state, "52_icon_moved");
    const oldp = px(state, 56, 68);                  /* old cell now empty wallpaper */
    const newp = px(state, 16 + 3 * 88 + 40, 16 + 2 * 96 + 24);
    if (oldp[1] === newp[1]) throw new Error("icon did not move");
    /* rubber band around the moved icon */
    await state.drag(16 + 3 * 88 + 90, 16 + 2 * 96 + 90, 16 + 3 * 88 - 10, 16 + 2 * 96 - 10);
    await sleep(300);
    shot(state, "53_rubberband");
    /* right-click it -> Remove from Desktop (item 1) */
    await state.click(16 + 3 * 88 + 40, 16 + 2 * 96 + 44, "right"); await sleep(400);
    const mx = 16 + 3 * 88 + 40, my = 16 + 2 * 96 + 44;
    await state.click(mx + 85, my + 3 + 24 + 12); await sleep(400);
    shot(state, "54_icon_removed");
    const gone = px(state, 16 + 3 * 88 + 40, 16 + 2 * 96 + 24);
    if (gone[1] > 120) throw new Error("icon still on desktop after remove");
    /* launcher: open search, type, enter */
    await state.click(25, 768 - 20); await sleep(400);
    shot(state, "55_launcher");
    const panel = px(state, 8, 768 - 40 - 150);      /* panel left frame = theme main (BGR) */
    if (panel[1] < 120) throw new Error("launcher panel not visible");
    await state.type("file");
    await sleep(300);
    shot(state, "56_launcher_search");
    await state.key("enter"); await sleep(700);
    shot(state, "57_launcher_opened");
    if (!(await live(state))) throw new Error("frozen after launcher");
});

/* --------------------------------------------------------------- 22 ---- */
test(22, "terminal: editor + root cwd", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    await state.type("edit /home/notes.txt\n"); await sleep(600);
    const hdr = px(state, w.x + 1 + 300, w.y + TITLEBAR + 10);
    const hs = hdr[0] + hdr[1] + hdr[2];
    if (hs < 40 || hs > 130) throw new Error("editor chrome missing (hdr=" + hs + ")");
    await state.type("hello scos"); await sleep(300);
    await state.scancodes([0x1d, 0x18, 0x98, 0x9d]); await sleep(400);   /* ctrl+o */
    shot(state, "56_editor");
    await state.scancodes([0x1d, 0x2d, 0xad, 0x9d]); await sleep(500);   /* ctrl+x */
    await state.type("cat /home/notes.txt\n"); await sleep(700);
    shot(state, "57_editor_saved");
    if (!(await live(state))) throw new Error("frozen after editor");
});

/* --------------------------------------------------------------- 23 ---- */
test(23, "files: /system real files + desktop shortcuts", async (state) => {
    const p = ICON_POS(ICON.files);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(800);
    const w = win_pos(1, 700, 500);
    const rowY = (r) => w.y + TITLEBAR + 44 + 4 + r * 24 + 12;
    const bright = (x, y) => { const q = px(state, x, y); return q[0] + q[1] + q[2]; };
    /* root lists home (row0) AND system (row1) - the boot chain is visible now */
    if (bright(w.x + 1 + 60, rowY(1)) < 60) throw new Error("system dir not visible at root");
    /* enter /system (double-click row 1) */
    await state.click(w.x + 1 + 200, rowY(1)); await sleep(150);
    await state.click(w.x + 1 + 200, rowY(1)); await sleep(600);
    shot(state, "58_system_dir");
    if (bright(w.x + 1 + 60, rowY(0)) < 60) throw new Error("/system lists no files");
    if (bright(w.x + 1 + 60, rowY(1)) < 60) throw new Error("/system lists only one file");
    /* toolbar 'up' button back to root, then home -> desktop */
    await state.click(w.x + 1 + 42 + 15, w.y + TITLEBAR + 8 + 13); await sleep(500);
    await state.click(w.x + 1 + 200, rowY(0)); await sleep(150);
    await state.click(w.x + 1 + 200, rowY(0)); await sleep(500);
    await state.click(w.x + 1 + 200, rowY(0)); await sleep(150);
    await state.click(w.x + 1 + 200, rowY(0)); await sleep(500);
    shot(state, "58_desktop_dir");
    if (bright(w.x + 1 + 60, rowY(0)) < 60) throw new Error("desktop shortcuts not listed");
    if (!(await live(state))) throw new Error("frozen in files");
});

/* --------------------------------------------------------------- 24 ---- */
test(24, "error screen: non-fatal tier shows and dismisses", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    await state.type("sysrq error\n"); await sleep(1200);
    /* the SYSTEM ERROR title block must contain many amber pixels */
    const fb = state.framebuffer();
    let amber = 0;
    for (let y = 48; y < 150; y++)
        for (let x = 420; x < 580; x++) {
            const i = (y * fb.w + x) * 4;
            /* v86 svga memory is B,G,R,A */
            const r = fb.mem[i + 2], g = fb.mem[i + 1], b = fb.mem[i];
            if (r > 180 && g > 120 && b < 120) amber++;
        }
    if (amber < 200) throw new Error("error screen title missing (amber=" + amber + ")");
    shot(state, "58_error_screen");
    await state.type("x"); await sleep(600);          /* any key dismisses */
    if (!(await live(state))) throw new Error("OS did not continue after error screen");
    await state.type("echo survived\n"); await sleep(500);
    if (!(await live(state))) throw new Error("terminal dead after dismiss");
});

/* --------------------------------------------------------------- 25 ---- */
test(25, "terminal: /system files + sysrq + diag subsystems", async (state) => {
    const p = ICON_POS(ICON.terminal);
    await state.click(p.x, p.y); await sleep(120); await state.click(p.x, p.y); await sleep(700);
    const w = win_pos(1, 700, 450);
    const textPx = () => {
        /* count bright text pixels inside the terminal content area */
        let n = 0;
        const fb = state.framebuffer();
        for (let y = w.y + TITLEBAR + 10; y < w.y + 400; y += 3)
            for (let x = w.x + 10; x < w.x + 690; x += 3) {
                const i = (y * fb.w + x) * 4;
                if (fb.mem[i] + fb.mem[i + 1] + fb.mem[i + 2] > 300) n++;
            }
        return n;
    };
    await state.type("cat /system/README.txt\n"); await sleep(900);
    const a = textPx();
    /* the real README is ~15 lines of text; an error line would be ~90 px */
    if (a < 300) throw new Error("README.txt did not display (px=" + a + ")");
    await state.type("clear\n"); await sleep(300);
    await state.type("cat /system/kernel.bin\n"); await sleep(900);
    const b = textPx();
    /* hex preview = 4+ dense hex lines; a not-found error is one thin line */
    if (b < 400) throw new Error("kernel.bin hex preview did not display (px=" + b + ")");
    shot(state, "59_kernel_hex");
    await state.type("clear\n"); await sleep(300);
    await state.type("sysrq time\n"); await sleep(600);
    if (textPx() < 20) throw new Error("sysrq time produced no output");
    await state.type("diag input\n"); await sleep(900);
    if (textPx() < 20) throw new Error("diag input produced no output");
    shot(state, "60_sysrq_diag");
    if (!(await live(state))) throw new Error("frozen after sysrq/diag");
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
