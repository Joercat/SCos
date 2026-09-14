/*
 * SCos native - live preview server.
 *
 * Serves a small web page that runs the SCos disk image in the v86 x86
 * emulator entirely inside your browser, with real mouse and keyboard
 * input. Nothing is installed globally: the emulator runtime is fetched
 * once into .preview/vendor by tools/setup_preview.sh.
 *
 *   node tools/preview_server.mjs [port]      (default 8080)
 *
 * Then open the printed URL. Click inside the screen to grab the keyboard;
 * press Esc or click outside to release it.
 */
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");
const VENDOR = path.join(REPO, ".preview", "vendor");
const IMG = path.join(REPO, "build", "scos.img");
const PORT = Number(process.argv[2] || process.env.PORT || 8080);

for (const f of ["libv86.js", "v86.wasm", "seabios.bin", "vgabios.bin"]) {
    if (!fs.existsSync(path.join(VENDOR, f))) {
        console.error("missing .preview/vendor/" + f + " - run tools/setup_preview.sh first");
        process.exit(1);
    }
}
if (!fs.existsSync(IMG)) {
    console.error("missing build/scos.img - run make first");
    process.exit(1);
}

const TYPES = {
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".wasm": "application/wasm",
    ".bin": "application/octet-stream",
    ".img": "application/octet-stream",
    ".html": "text/html; charset=utf-8",
};

const PAGE = `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>SCos native - live preview</title>
<style>
  html, body { margin: 0; height: 100%; background: #101010; color: #c8ffc8;
               font: 14px/1.5 ui-monospace, Menlo, Consolas, monospace; }
  header { padding: 10px 16px; display: flex; gap: 12px; align-items: center;
           background: #060f06; border-bottom: 1px solid #1d4d1d; }
  header b { color: #39ff14; }
  header .sp { flex: 1; }
  button { background: #123312; color: #c8ffc8; border: 1px solid #39ff14;
           padding: 4px 12px; font: inherit; cursor: pointer; }
  button:hover { background: #39ff14; color: #060f06; }
  #wrap { display: flex; justify-content: center; padding: 14px; }
  #screen_container { width: 1024px; height: 768px; background: #000;
                      box-shadow: 0 0 40px #000; cursor: none; }
  #screen_container canvas { display: block; }
  #hint { padding: 0 16px 16px; color: #7fbf7f; max-width: 1024px; margin: 0 auto; }
  #status { color: #ffd75f; }
</style>
</head>
<body>
<header>
  <b>SCos 2.0</b> native kernel &mdash; live preview
  <span class="sp"></span>
  <span id="status">starting...</span>
  <button id="btn_restart">Restart</button>
  <button id="btn_full">Fullscreen</button>
</header>
<div id="wrap"><div id="screen_container"></div></div>
<div id="hint">
  Click inside the screen to capture keyboard &amp; mouse. The machine boots
  from the real disk image (MBR bootloader &rarr; protected-mode kernel).
  Try: double-click is not needed &mdash; one click opens desktop icons; drag
  windows by their title bar; right-click files for a context menu;
  <code>help</code> in the Terminal lists every command.
</div>
<script src="/vendor/libv86.js"></script>
<script>
const status = document.getElementById("status");
let emu = null;
function start() {
    if (emu) { try { emu.destroy(); } catch (e) {} }
    status.textContent = "booting...";
    emu = new V86({
        bios: { url: "/vendor/seabios.bin" },
        vga_bios: { url: "/vendor/vgabios.bin" },
        hda: { url: "/scos.img" },
        memory_size: 256 * 1024 * 1024,
        wasm_path: "/vendor/v86.wasm",
        screen_container: document.getElementById("screen_container"),
        autostart: true,
    });
    emu.add_listener("screen-set-mode", () => { status.textContent = "running"; });
    setTimeout(() => { if (status.textContent === "booting...") status.textContent = "running"; }, 4000);
}
document.getElementById("btn_restart").onclick = () => { start(); };
document.getElementById("btn_full").onclick = () => {
    const el = document.getElementById("screen_container");
    (el.requestFullscreen || el.webkitRequestFullscreen || function(){}).call(el);
};
start();
</script>
</body>
</html>
`;

const server = http.createServer((req, res) => {
    const url = new URL(req.url, "http://x");
    let file = null;
    if (url.pathname === "/" || url.pathname === "/index.html") {
        res.writeHead(200, { "Content-Type": TYPES[".html"], "Cache-Control": "no-store" });
        res.end(PAGE);
        return;
    }
    if (url.pathname.startsWith("/vendor/"))
        file = path.join(VENDOR, path.basename(url.pathname));
    else if (url.pathname === "/scos.img")
        file = IMG;
    if (!file || !fs.existsSync(file)) {
        res.writeHead(404); res.end("not found");
        return;
    }
    const ext = path.extname(file);
    res.writeHead(200, {
        "Content-Type": TYPES[ext] || "application/octet-stream",
        "Content-Length": fs.statSync(file).size,
        "Cache-Control": ext === ".img" ? "no-store" : "public, max-age=3600",
    });
    fs.createReadStream(file).pipe(res);
});

server.listen(PORT, "0.0.0.0", () => {
    console.log("SCos preview serving on http://0.0.0.0:" + PORT + "/");
});
