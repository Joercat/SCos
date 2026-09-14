/* Debug: type a battery of terminal commands and capture screens for text decoding. */
import fs from "node:fs";
import { boot, sleep } from "./harness.mjs";
fs.mkdirSync(new URL("./../build/tests/", import.meta.url).pathname, { recursive: true });

const OUT = new URL("./../build/tests/", import.meta.url).pathname;
const TITLEBAR = 26;
function win_pos(n, dw, dh) {
    return {
        x: 40 + (n * 28) % (1024 - dw - 60),
        y: 24 + (n * 26) % (768 - 40 - dh - 40),
    };
}
const ICON_POS = (i) => ({ x: 16 + (i % 8) * 88 + 40, y: 16 + Math.floor(i / 8) * 96 + 44 });

const state = await boot("build/scos.img");
await sleep(4500);
const p = ICON_POS(1);
await state.click(p.x, p.y); await sleep(700);
const w = win_pos(1, 700, 450);

const batches = process.argv[2] === "raw" ? [9] : process.argv[2] ? [Number(process.argv[2])] : [1, 2, 3];
const cmds_raw = process.argv.slice(3);
var cmds = {
    1: [
        "ls", "pwd", "cd documents", "ls", "touch a.txt", "ls", "rm a.txt", "ls",
        "cd ..", "ls", "cat documents/welcome.txt", "wc documents/welcome.txt",
        "grep SCos documents/welcome.txt", "cp documents/welcome.txt documents/copy.txt",
        "mv documents/copy.txt documents/moved.txt", "ls documents",
    ],
    2: [
        "clear", "head documents/welcome.txt 3", "hexdump documents/welcome.txt",
        "tree", "free", "cpu", "df", "disks", "uptime", "theme", "cal",
    ],
    3: [
        "clear", "neofetch", "sysinfo", "calc 2 + 3", "date", "history", "help",
    ],
};
cmds[9] = cmds_raw;
for (const b of batches) {
    for (const c of cmds[b]) {
        if (c.startsWith("SHOT:")) { state.save_png(OUT + c.slice(5) + ".ppm"); continue; }
        await state.type(c + "\n");
        await sleep(c === "clear" ? 300 : 900);
    }
    state.save_png(OUT + "dbg_" + b + ".ppm");
}
state.destroy();
console.log("debug done");
