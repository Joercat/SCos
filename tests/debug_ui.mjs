import fs from "node:fs";
import { boot, sleep } from "./harness.mjs";
fs.mkdirSync(new URL("./../build/tests/", import.meta.url).pathname, { recursive: true });
const OUT = new URL("./../build/tests/", import.meta.url).pathname;
const ICON_POS = (i) => ({ x: 16 + (i % 8) * 88 + 40, y: 16 + Math.floor(i / 8) * 96 + 44 });
const s = await boot("build/scos.img");
await sleep(4500);
let p = ICON_POS(1);
await s.click(p.x, p.y); await sleep(700);
for (const app of ["files", "notepad", "calendar", "about", "blackjack", "browser", "sysmon"]) {
    await s.type("open " + app + "\n"); await sleep(600);
    await s.click(60, 754); await sleep(300);      /* refocus terminal via taskbar */
}
s.save_png(OUT + "dbg_sysmon.ppm");
/* select files row: wins order terminal,files,notepad,calendar,about,blackjack,browser,sysmon -> files = row 6 */
await s.click(180 + 1 + 200, 154 + 26 + 222 + 6 * 18 + 8); await sleep(400);
s.save_png(OUT + "dbg_sysmon_sel.ppm");
/* End Task on notepad */
const sw = { x: 40 + (7 * 28) % (1024 - 640 - 60), y: 24 + (7 * 26) % (768 - 40 - 480 - 40) };
await s.click(sw.x + 1 + 67, sw.y + 26 + 480 - 36 + 13); await sleep(600);
s.save_png(OUT + "dbg_taskbar_full.ppm");
await s.move(400, 748); await s.wheel(2); await sleep(400);
s.save_png(OUT + "dbg_taskbar_scrolled.ppm");
await s.click(1024 - 68 - 34 - 16 + 17, 768 - 40 + 20); await sleep(500);
s.save_png(OUT + "dbg_powermenu.ppm");
s.destroy();
console.log("ui debug done");
