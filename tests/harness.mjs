/*
 * SCos native - headless verification harness (v86).
 *
 * Boots build/scos.img in the v86 x86 emulator with a serial console,
 * framebuffer capture and PS/2 input injection.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");

/* vendor assets: prefer the in-repo copy (tools/setup_preview.sh), fall back
 * to a developer-local v86 checkout */
const CANDIDATES = [
    path.join(REPO, ".preview", "vendor"),
    "/home/user/emutest/node_modules/v86/build",
];
function vendor_file(name) {
    const extra = name.endsWith(".bin") ? ["", "../bios/"] : [];
    for (const c of CANDIDATES) {
        for (const p of [path.join(c, name), path.join(c, "..", "bios", name)]) {
            if (fs.existsSync(p)) return p;
        }
    }
    throw new Error("missing v86 asset " + name + " - run tools/setup_preview.sh");
}
const LIBV86 = fs.existsSync(path.join(REPO, ".preview", "vendor", "libv86.mjs"))
    ? path.join(REPO, ".preview", "vendor", "libv86.mjs")
    : vendor_file("libv86.mjs");
const { V86 } = await import(LIBV86);

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function boot(imgPath, opts = {})
{
    const img = new Uint8Array(fs.readFileSync(imgPath));
    const emulator = new V86({
        bios: { url: vendor_file("seabios.bin") },
        vga_bios: { url: vendor_file("vgabios.bin") },
        hda: { buffer: img.buffer },
        memory_size: opts.mem_mb ? opts.mem_mb * 1024 * 1024 : 256 * 1024 * 1024,
        wasm_path: vendor_file("v86.wasm"),
        disable_mouse: true,
        disable_keyboard: true,
        autostart: true,
    });

    const state = {
        emulator,
        serial: "",
        cursor: { x: 512, y: 384 },   /* wm starts the cursor at screen centre */
    };
    emulator.add_listener("serial0-output-byte", (b) => {
        state.serial += String.fromCharCode(b);
    });

    state.wait = sleep;

    state.vga = () => emulator.v86.cpu.devices.vga;

    state.framebuffer = () => {
        const vga = state.vga();
        return {
            w: vga.svga_width, h: vga.svga_height,
            mem: vga.svga_memory, bpp: vga.svga_bpp,
        };
    };

    state.pixel = (x, y) => {
        const fb = state.framebuffer();
        const i = (y * fb.w + x) * 4;
        return [fb.mem[i + 2], fb.mem[i + 1], fb.mem[i + 0]];   /* R,G,B */
    };

    state.save_png = (path) => {
        const fb = state.framebuffer();
        const w = fb.w, h = fb.h;
        const header = Buffer.from(`P6\n${w} ${h}\n255\n`, "binary");
        const buf = Buffer.alloc(header.length + w * h * 3);
        header.copy(buf, 0);
        for (let i = 0, j = header.length; i < w * h; i++) {
            buf[j++] = fb.mem[i * 4 + 2];
            buf[j++] = fb.mem[i * 4 + 1];
            buf[j++] = fb.mem[i * 4 + 0];
        }
        fs.writeFileSync(path, buf);
    };

    state.move = async (x, y) => {
        let dx = x - state.cursor.x, dy = y - state.cursor.y;
        /* PS/2 movement packets are 8-bit signed: chunk the delta */
        while (dx || dy) {
            const sx = Math.max(-100, Math.min(100, dx));
            const sy = Math.max(-100, Math.min(100, dy));
            emulator.bus.send("mouse-delta", [sx, -sy]);  /* PS/2: +dy = up, like v86 browser adapter */
            dx -= sx; dy -= sy;
            state.cursor.x += sx; state.cursor.y += sy;
            await sleep(12);
        }
        await sleep(30);
    };

    state.click = async (x, y, button = "left") => {
        await state.move(x, y);
        const idx = button === "left" ? 0 : button === "middle" ? 1 : 2;
        const down = [false, false, false];
        down[idx] = true;
        emulator.bus.send("mouse-click", down);
        await sleep(40);
        emulator.bus.send("mouse-click", [false, false, false]);
        await sleep(60);
    };

    state.wheel = async (notches) => {
        emulator.bus.send("mouse-wheel", [notches, 0]);
        await sleep(80);
    };

    state.drag = async (x1, y1, x2, y2, button = "left") => {
        await state.move(x1, y1);
        const idx = button === "left" ? 0 : button === "middle" ? 1 : 2;
        const down = [false, false, false];
        down[idx] = true;
        emulator.bus.send("mouse-click", down);
        await sleep(50);
        let dx = x2 - x1, dy = y2 - y1;
        while (dx || dy) {
            const sx = Math.max(-100, Math.min(100, dx));
            const sy = Math.max(-100, Math.min(100, dy));
            emulator.bus.send("mouse-delta", [sx, -sy]);  /* PS/2: +dy = up, like v86 browser adapter */
            dx -= sx; dy -= sy;
            state.cursor.x += sx; state.cursor.y += sy;
            await sleep(15);
        }
        await sleep(50);
        emulator.bus.send("mouse-click", [false, false, false]);
        await sleep(60);
    };

    /* US layout set-1 scancodes; shifted variants for symbols/caps */
    const KEYMAP = {
        "a":0x1e,"b":0x30,"c":0x2e,"d":0x20,"e":0x12,"f":0x21,"g":0x22,"h":0x23,"i":0x17,
        "j":0x24,"k":0x25,"l":0x26,"m":0x32,"n":0x31,"o":0x18,"p":0x19,"q":0x10,"r":0x13,
        "s":0x1f,"t":0x14,"u":0x16,"v":0x2f,"w":0x11,"x":0x2d,"y":0x15,"z":0x2c,
        "0":0x0b,"1":0x02,"2":0x03,"3":0x04,"4":0x05,"5":0x06,"6":0x07,"7":0x08,"8":0x09,"9":0x0a,
        " ":0x39,"-":0x0c,"=":0x0d,"[":0x1a,"]":0x1b,"\\":0x2b,";":0x27,"'":0x28,"`":0x29,
        ",":0x33,".":0x34,"/":0x35,
    };
    const SHIFTED = {
        ")":"0","!":"1","@":"2","#":"3","$":"4","%":"5","^":"6","&":"7","*":"8","(":"9",
        "_":"-","+":"=","{":"[","}":"]","|":"\\",":":";","\"":"'","~":"`","<":",",">":".","?":"/",
    };
    state.type = async (text) => {
        for (const ch of text) {
            let c = ch, shift = false;
            if (SHIFTED[c]) { c = SHIFTED[c]; shift = true; }
            else if (c >= "A" && c <= "Z") { c = c.toLowerCase(); shift = true; }
            const sc = KEYMAP[c];
            if (c === "\n") { emulator.keyboard_send_scancodes([0x1c, 0x9c]); await sleep(30); continue; }
            if (c === "\b") { emulator.keyboard_send_scancodes([0x0e, 0x8e]); await sleep(30); continue; }
            if (!sc) continue;
            const seq = shift ? [0x2a, sc, sc | 0x80, 0xaa] : [sc, sc | 0x80];
            emulator.keyboard_send_scancodes(seq);
            await sleep(18);
        }
        await sleep(60);
    };

    state.scancodes = async (arr) => {
        emulator.keyboard_send_scancodes(arr);
        await sleep(40);
    };
    state.key = async (name) => {
        const codes = {
            enter: [0x1c, 0x9c],
            backspace: [0x0e, 0x8e],
            up: [0x48, 0xc8],
            down: [0x50, 0xd0],
            left: [0x4b, 0xcb],
            right: [0x4d, 0xcd],
            esc: [0x01, 0x81],
            tab: [0x0f, 0x8f],
        };
        const c = codes[name];
        if (c) await emulator.keyboard_send_scancodes(c);
        await sleep(40);
    };

    state.destroy = () => emulator.destroy();
    return state;
}

export function wait_for(state, predicate, timeout_ms = 20000)
{
    return new Promise((resolve, reject) => {
        const t0 = Date.now();
        const iv = setInterval(() => {
            if (predicate(state)) { clearInterval(iv); resolve(true); }
            else if (Date.now() - t0 > timeout_ms) { clearInterval(iv); reject(new Error("timeout")); }
        }, 100);
    });
}
