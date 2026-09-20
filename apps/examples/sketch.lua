-- A resize-aware canvas. Left-click to add a mark; R clears the drawing.
local marks = {}
return {
    open = function() scos.title("Lua Sketch") end,
    paint = function(w, h)
        scos.clear(0x171324)
        scos.text(16, 16, "Click to draw. R clears. Resize freely.", 0xcc88ff)
        for _, p in ipairs(marks) do
            scos.rect(math.floor(p.x * w), math.floor(p.y * h), 6, 6, p.c)
        end
        scos.text(16, h - 24, tostring(#marks) .. " marks", 0xffffff)
    end,
    mouse = function(kind, x, y, button, down)
        if kind == 2 and button == 1 and down and #marks < 1000 then
            local w, h = scos.size()
            marks[#marks + 1] = {x = x / w, y = y / h, c = math.random(0x448888, 0xffffff)}
            scos.redraw()
        end
    end,
    key = function(code, down)
        if down and (code == 114 or code == 82) then marks = {}; scos.redraw() end
    end
}
