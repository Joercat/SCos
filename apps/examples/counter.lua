-- Studio template: build sample-counter.cat. The built-in Counter is C.
-- Mouse/keyboard state belongs to this window, not to the compositor.
local count = 0
local function increment()
    count = count + 1
    scos.redraw()
end
return {
    open = function()
        scos.title("Lua Counter")
        count = tonumber(scos.read("count.txt")) or 0
        scos.log("Lua counter compiled and started")
    end,
    paint = function(w, h)
        scos.clear(0x101820)
        scos.text(20, 20, "Your own app, written in Lua", 0x39ff14)
        scos.text(20, 52, "Count: " .. count, 0xffffff)
        scos.rect(20, 84, math.max(0, math.min(w - 40, 240)), 40, 0x17402a)
        scos.text(32, 96, "Click here or press Space", 0xffffff)
        scos.text(20, 148, "Press S to save in the RAM filesystem.", 0xaaaaaa)
        scos.text(20, 172, "Use terminal save for supported disks.", 0xaaaaaa)
    end,
    mouse = function(kind, x, y, button, down)
        if kind == 2 and button == 1 and down and x >= 20 and x < 260 and y >= 84 and y < 124 then
            increment()
        end
    end,
    key = function(code, down)
        if not down then return end
        if code == 32 then increment() end
        if code == 115 or code == 83 then
            assert(scos.write("count.txt", tostring(count)), "save failed")
            scos.log("Counter saved to /home/appdata/counter/count.txt (RAM)")
        end
    end
}
