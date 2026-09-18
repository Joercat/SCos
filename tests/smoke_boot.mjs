/* Quick boot smoke test: serial log + desktop screenshot */
import { boot, sleep } from "./harness.mjs";

const state = await boot("build/scos.img");
try {
    await sleep(12000);
    console.log("---- SERIAL ----");
    console.log(state.serial);
    state.save_png("build/shot_boot.ppm");
    console.log("---- framebuffer ----");
    const fb = state.framebuffer();
    console.log("res:", fb.w + "x" + fb.h, "bpp", fb.bpp);
    console.log("pixel(512,400):", state.pixel(512, 400));
    console.log("pixel(10,760):", state.pixel(10, 760));
} finally {
    state.destroy();
    process.exit(0);
}
