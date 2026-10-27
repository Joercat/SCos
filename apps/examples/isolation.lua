-- Isolation test: an app whose whole purpose is to try to break out of its
-- own sandbox and say, on screen and in a report file, what happened.
--
-- Run it isolated (right-click its icon or taskbar row -> "Launch isolated",
-- or choices app isolation always).  Every probe below is something a hostile
-- package would try: leave the private data directory, read another account or
-- another app, take over the system theme, raise notifications and dialogs
-- without permission, exhaust the VM, reach the Lua libraries that touch files,
-- or draw outside the paint callback.  A probe is CONTAINED when the call is
-- refused; it is a BREAKOUT when the call is allowed.
--
-- One CAT event is one fresh Lua state, so progress lives in data files: each
-- event runs a few probes, appends the verdicts to results.txt and advances
-- cursor.txt.  The aggregated report is written to report.txt for the build's
-- regression suite (tools/tests/test_escape.py), which fails on any breakout.

local BATCH = 4                 -- probes per event: every callback has a budget
local CURSOR, RESULTS, REPORT = "cursor.txt", "results.txt", "report.txt"

local probes = {}

-- ---------------------------------------------------------------- helpers ---
-- A refusal can be a Lua error (capability guard, namespace validator) or a
-- false/nil return.  Both are contained; anything else is a breakout.
local function must_refuse(name, fn)
    local ok, a = pcall(fn)
    if not ok then return name .. "=CONTAINED" end
    if a == nil or a == false then return name .. "=CONTAINED" end
    return name .. "=BREAKOUT allowed, returned " .. tostring(a)
end

-- The control probe: the sandbox must still let the app use its OWN data.
local function must_allow(name, fn)
    local ok, a = pcall(fn)
    if not ok then return name .. "=BREAKOUT own data refused: " .. tostring(a) end
    if a == nil or a == false then return name .. "=BREAKOUT own data refused" end
    return name .. "=CONTAINED"
end

local function has_field(name) return rawget(_G, name) ~= nil end

-- ------------------------------------------------- leaving the namespace ----
probes[#probes + 1] = function() return must_refuse("traversal_write", function() return scos.write("../escape.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("traversal_write2", function() return scos.write("../../home/escape.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("absolute_write", function() return scos.write("/home/escape.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("other_app_write", function() return scos.write("../notepad/steal.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("subdir_write", function() return scos.write("sub/evil.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("dot_name", function() return scos.write(".", "x") end) end
probes[#probes + 1] = function() return must_refuse("dotdot_name", function() return scos.write("..", "x") end) end
probes[#probes + 1] = function() return must_refuse("backslash_name", function() return scos.write("..\\..\\evil.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("nul_name", function() return scos.write("ok\0../evil.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("long_name", function() return scos.write(string.rep("a", 300), "x") end) end
probes[#probes + 1] = function() return must_refuse("traversal_read", function() return scos.read("../../../etc/passwd") end) end
probes[#probes + 1] = function() return must_refuse("absolute_read", function() return scos.read("/home") end) end
probes[#probes + 1] = function() return must_refuse("other_account_read", function() return scos.read("home/users/root/notes") end) end
probes[#probes + 1] = function() return must_refuse("traversal_exists", function() return scos.exists("../notepad") end) end
probes[#probes + 1] = function() return must_refuse("traversal_size", function() return scos.file_size("../../secret") end) end
probes[#probes + 1] = function() return must_refuse("traversal_remove", function() return scos.remove("../notepad/notes") end) end
probes[#probes + 1] = function() return must_refuse("traversal_rename", function() return scos.rename("own.txt", "../escape.txt") end) end
probes[#probes + 1] = function() return must_refuse("rename_to_root", function() return scos.rename("own.txt", "/escape.txt") end) end

-- The listing may only contain the app's own plain file names.
probes[#probes + 1] = function()
    -- Listing your OWN directory has to work; an error here is an API bug,
    -- not containment.
    local ok, list = pcall(scos.files)
    if not ok then return "files_listing=BREAKOUT own listing refused: " .. tostring(list) end
    for _, n in ipairs(list) do
        if n == ".." or n == "." or string.find(n, "/") then
            return "files_listing=BREAKOUT listed " .. tostring(n)
        end
    end
    return "files_listing=CONTAINED"
end

-- --------------------------------- the private directory must work --------
probes[#probes + 1] = function() return must_allow("own_write", function() return scos.write("own.txt", "sandbox ok") end) end
probes[#probes + 1] = function() return must_allow("own_read", function() return scos.read("own.txt") end) end
probes[#probes + 1] = function() return must_allow("own_exists", function() return scos.exists("own.txt") end) end
probes[#probes + 1] = function() return must_allow("own_size", function() return scos.file_size("own.txt") end) end
probes[#probes + 1] = function() return must_allow("own_rename", function() return scos.rename("own.txt", "renamed.txt") end) end
probes[#probes + 1] = function() return must_allow("own_remove", function() return scos.remove("renamed.txt") end) end

-- -------------------------------------------------- global capabilities ----
probes[#probes + 1] = function() return must_refuse("theme_apply", function() return scos.theme_apply("blue-sky") end) end
probes[#probes + 1] = function() return must_refuse("theme_custom", function() return scos.theme_custom("evil", {}) end) end
probes[#probes + 1] = function()
    -- Reading the theme is allowed; changing it must not be.
    local id = pcall(function() return scos.theme().id end)
    if not id then return "theme_read=BREAKOUT theme query failed" end
    return "theme_read=CONTAINED"
end
probes[#probes + 1] = function() return must_refuse("notify", function() return scos.notify("escaped") end) end
probes[#probes + 1] = function() return must_refuse("message", function() return scos.message("escaped") end) end
probes[#probes + 1] = function() return must_refuse("set_icon", function() return scos.set_icon(9) end) end

-- -------------------------------- the guard must live in the kernel -----
probes[#probes + 1] = function()
    -- Replace the API function the package itself calls.  If the check were
    -- implemented in Lua, this would hand the app a permissive stub; the real
    -- capability check must still refuse the saved original.
    local real = scos.write
    scos.write = function() return true end
    local ok, r = pcall(real, "../stub_escape.txt", "x")
    scos.write = real
    if ok and r ~= false and r ~= nil then
        return "api_stub=BREAKOUT a Lua stub replaced the kernel check"
    end
    return "api_stub=CONTAINED"
end
probes[#probes + 1] = function()
    -- A hostile __index on the globals table must not intercept scos either.
    local mt = getmetatable(_G)
    setmetatable(_G, { __index = function() return nil end })
    local ok, r = pcall(scos.write, "../meta_escape.txt", "x")
    setmetatable(_G, mt)
    if ok and r ~= false and r ~= nil then
        return "global_metatable=BREAKOUT"
    end
    return "global_metatable=CONTAINED"
end
probes[#probes + 1] = function()
    -- Deep (bounded) Lua recursion must stay inside the VM heap.  An unbounded
    -- loop is a CPU-budget case, and is tested by killing a spinning package
    -- from the suite instead of from inside this app.
    local function deep(n) if n <= 0 then return 0 end return 1 + deep(n - 1) end
    local ok = pcall(deep, 1000)
    if not ok then return "lua_recursion=CONTAINED raised" end
    return "lua_recursion=CONTAINED 1000 frames"
end
probes[#probes + 1] = function()
    -- C-stack recursion: a gsub callback that calls gsub again.
    local function boom() return (string.gsub("x", "x", boom)) end
    local ok = pcall(boom)
    if ok then return "c_recursion=BREAKOUT no C stack guard" end
    return "c_recursion=CONTAINED"
end

-- --------------------------------- argument validation / resource limits ---
probes[#probes + 1] = function() return must_refuse("read_dot", function() return scos.read(".") end) end
probes[#probes + 1] = function() return must_refuse("read_dotdot", function() return scos.read("..") end) end
probes[#probes + 1] = function() return must_refuse("interval_low", function() return scos.interval(0) end) end
probes[#probes + 1] = function() return must_refuse("notify_long", function() return scos.notify(string.rep("n", 5000)) end) end
probes[#probes + 1] = function() return must_refuse("text_width_long", function() return scos.text_width(string.rep("t", 2000)) end) end
probes[#probes + 1] = function() return must_refuse("rgb_range", function() return scos.rgb(999, 0, 0) end) end
probes[#probes + 1] = function() return must_refuse("blend_range", function() return scos.blend(0, 0, -5) end) end
probes[#probes + 1] = function() return must_refuse("clamp_reversed", function() return scos.clamp(1, 5, 0) end) end
probes[#probes + 1] = function() return must_refuse("hit_extent", function() return scos.hit(0, 0, 0, 0, 99999, 99999) end) end
probes[#probes + 1] = function()
    -- A 100 MB string must be refused by the VM, not by the machine dying.
    local ok = pcall(function() return string.rep("a", 100 * 1000 * 1000) end)
    if ok then return "huge_string=BREAKOUT 100 MB allocated" end
    return "huge_string=CONTAINED"
end
probes[#probes + 1] = function() return must_refuse("huge_resize", function() return scos.resize(999999, 999999) end) end
probes[#probes + 1] = function()
    -- Moving cannot put the window off the screen.
    pcall(scos.move, -99999, -99999)
    local ok, w = pcall(scos.window)
    if not ok then return "move_clamped=CONTAINED" end
    if w.x >= 0 and w.y >= 0 then return "move_clamped=CONTAINED" end
    return "move_clamped=BREAKOUT x=" .. tostring(w.x) .. " y=" .. tostring(w.y)
end
probes[#probes + 1] = function() return must_refuse("long_title", function() return scos.title(string.rep("T", 500)) end) end
probes[#probes + 1] = function() return must_refuse("long_log", function() return scos.log(string.rep("L", 600)) end) end
probes[#probes + 1] = function()
    -- The 16-file data quota must hold.
    local made, refused = 0, false
    for i = 1, 20 do
        local ok, r = pcall(scos.write, "q" .. i .. ".txt", "x")
        if ok and r then made = made + 1 else refused = true end
    end
    for i = 1, 20 do pcall(scos.remove, "q" .. i .. ".txt") end
    if refused then return "data_quota=CONTAINED " .. made .. " files accepted" end
    return "data_quota=BREAKOUT no refusal after " .. made
end
probes[#probes + 1] = function() return must_refuse("oversize_file", function() return scos.write("big.txt", string.rep("a", 100000)) end) end
probes[#probes + 1] = function()
    -- A memory bomb must fail inside the VM, not take the machine down.  The
    -- CPL3 host's whole Lua heap is 1 MiB, so 8 MiB has to be refused.
    local ok = pcall(function() return string.rep("a", 8 * 1024 * 1024) end)
    if ok then return "memory_bomb=BREAKOUT 8 MiB allocated from a 1 MiB arena" end
    return "memory_bomb=CONTAINED"
end
probes[#probes + 1] = function() return must_refuse("draw_outside_paint", function() return scos.clear(0) end) end

-- ------------------------------------------- the VM's own escape hatches ---
probes[#probes + 1] = function()
    local leaks = {}
    for _, name in ipairs({"load", "loadfile", "dofile", "require", "io", "os",
                           "package", "debug", "coroutine", "sc", "syscall"}) do
        if has_field(name) then leaks[#leaks + 1] = name end
    end
    if #leaks == 0 then return "vm_libraries=CONTAINED none present" end
    return "vm_libraries=BREAKOUT exposed " .. table.concat(leaks, ",")
end
probes[#probes + 1] = function()
    if rawget(string, "dump") == nil then return "string_dump=CONTAINED" end
    return "string_dump=BREAKOUT string.dump is available"
end
probes[#probes + 1] = function()
    local id = pcall(scos.app_id) and scos.app_id() or ""
    if id == "isolation" then return "app_id=CONTAINED" end
    return "app_id=BREAKOUT reported " .. tostring(id)
end
probes[#probes + 1] = function()
    -- Theme names must be real IDs, not list indices.
    local ok, list = pcall(scos.themes)
    if not ok or type(list) ~= "table" or #list < 1 then return "theme_names=BREAKOUT no theme list" end
    for _, id in ipairs(list) do
        if type(id) ~= "string" or id == "" then return "theme_names=BREAKOUT entry " .. tostring(id) end
    end
    return "theme_names=CONTAINED " .. #list .. " themes"
end
probes[#probes + 1] = function()
    local t = pcall(scos.theme) and scos.theme() or nil
    if t and type(t.id) == "string" and t.id ~= "" and type(t.win_bg) == "number" then
        return "theme_palette=CONTAINED"
    end
    return "theme_palette=BREAKOUT id or palette missing"
end
probes[#probes + 1] = function()
    local d = pcall(scos.date) and scos.date() or nil
    if d and d.year and d.month and d.day and d.weekday ~= nil and d.hour ~= nil then
        return "date_record=CONTAINED"
    end
    return "date_record=BREAKOUT incomplete record"
end
probes[#probes + 1] = function()
    local t = pcall(scos.time) and scos.time() or -1
    if type(t) == "number" and t >= 0 then return "clock=CONTAINED" end
    return "clock=BREAKOUT " .. tostring(t)
end
probes[#probes + 1] = function()
    local ok, w = pcall(scos.window)
    if ok and type(w) == "table" and w.width and w.height and w.state ~= nil then
        return "window_table=CONTAINED"
    end
    return "window_table=BREAKOUT incomplete window table"
end

-- These run inside paint, where drawing is legal.  The rect/disc/icon/text
-- ranges must be refused there too, and the whole per-episode pixel budget
-- must cut the episode off instead of hanging a CPU-rendered desktop.
local paint_probes = {
    function() return must_refuse("paint_rect_range", function() return scos.rect(0, 0, 999999, 999999, 0) end) end,
    function() return must_refuse("paint_disc_range", function() return scos.disc(0, 0, 100000, 0) end) end,
    function() return must_refuse("paint_icon_range", function() return scos.icon(999, 0, 0, 0) end) end,
    function() return must_refuse("paint_text_range", function() return scos.text(0, 0, string.rep("T", 2000), 0) end) end,
    function() return must_refuse("paint_pixel_range", function() return scos.pixel(99999, -99999, 0) end) end,
    function() return must_refuse("paint_gradient_range", function() return scos.gradient(0, 0, 99999, 99999, 0, 0) end) end,
    function() return must_refuse("paint_line_range", function() return scos.line(-99999, 0, 99999, 0, 0) end) end,
    function() return must_refuse("paint_wrap_long", function() return scos.text_wrap(0, 0, 200, 100, string.rep("W", 3000), 0) end) end,
    function() return must_refuse("paint_button_range", function() return scos.button(0, 0, 99999, 99999, "b") end) end,
    function()
        local err, accepted = nil, 0
        for _ = 1, 40 do
            local ok, e = pcall(scos.clear, 0)
            if not ok then err = e break end
            accepted = accepted + 1
        end
        if err and accepted >= 3 then return "paint_budget=CONTAINED " .. accepted .. " clears before the cap" end
        if err then return "paint_budget=CONTAINED" end
        return "paint_budget=BREAKOUT 40 full clears were all accepted"
    end,
}

-- ------------------------------------------------------------ bookkeeping ---
local function read_file(name)
    local ok, data = pcall(scos.read, name)
    if not ok or type(data) ~= "string" then return nil end
    return data
end

local function write_file(name, text)
    local ok, r = pcall(scos.write, name, text)
    return ok and r
end

local function append_line(name, line)
    local old = read_file(name) or ""
    if #old > 60000 then old = "" end
    return write_file(name, old .. line .. "\n")
end

local function verdicts()
    local text = read_file(RESULTS) or ""
    local contained, breakout, lines = 0, 0, {}
    for line in string.gmatch(text, "[^\n]+") do
        lines[#lines + 1] = line
        if string.find(line, "=BREAKOUT", 1, true) then breakout = breakout + 1
        else contained = contained + 1 end
    end
    return contained, breakout, lines
end

local function total_probes() return #probes + #paint_probes end

local function write_report()
    local contained, breakout, lines = verdicts()
    local head = { "isolation-report 1",
                   "total=" .. total_probes(),
                   "contained=" .. contained,
                   "breakout=" .. breakout }
    local text = table.concat(head, "\n") .. "\n"
    for _, line in ipairs(lines) do text = text .. line .. "\n" end
    write_file(REPORT, text)
    return contained, breakout
end

local function step()
    if read_file("done.txt") then return false end
    local at = tonumber(read_file(CURSOR) or "") or 1
    if at > #probes then return false end
    local last = at + BATCH - 1
    if last > #probes then last = #probes end
    for i = at, last do
        local ok, line = pcall(probes[i])
        if not ok then line = "probe_" .. i .. "=CONTAINED " .. tostring(line) end
        append_line(RESULTS, line)
    end
    write_file(CURSOR, tostring(last + 1))
    if last >= #probes then
        local c, b = write_report()
        scos.log(string.format("isolation: %d/%d probes contained, %d breakout",
                               c, total_probes(), b))
        return true
    end
    return true
end

local function run_paint_probes()
    if read_file("painted.txt") then return end
    if not read_file(CURSOR) then return end       -- tick side not started yet
    write_file("painted.txt", "1")
    for _, probe in ipairs(paint_probes) do
        local ok, line = pcall(probe)
        if not ok then line = "paint_probe=CONTAINED " .. tostring(line) end
        append_line(RESULTS, line)
    end
    local at = tonumber(read_file(CURSOR) or "") or 1
    if at > #probes then write_report() end
end

local function paint_summary(w, h)
    local t = scos.theme()
    local contained, breakout, lines = verdicts()
    local total = total_probes()
    local done = read_file("done.txt") ~= nil
    scos.clear(t.win_bg)
    local accent = breakout > 0 and 0xff4444 or t.main
    scos.text(16, 14, "Isolation test", accent)
    scos.text(16, 38, string.format("%d contained, %d breakout (%d probes)",
                                    contained, breakout, total), t.text)
    if breakout > 0 then
        scos.text(16, 62, "SEE results.txt - an escape succeeded", 0xff4444)
    elseif done then
        scos.text(16, 62, "No breakout found. Press R to run again.", accent)
    else
        local at = tonumber(read_file(CURSOR) or "") or 1
        scos.text(16, 62, string.format("running %d/%d...", math.min(at, total), total), t.text)
    end
    local y = 92
    local rows = math.floor(math.max(0, h - y - 28) / 16)
    for i = 1, math.min(#lines, rows) do
        local line = lines[i]
        local color = string.find(line, "=BREAKOUT", 1, true) and 0xff4444 or t.main
        scos.text(16, y, line, color)
        y = y + 16
    end
    if not done then
        local at = tonumber(read_file(CURSOR) or "") or 1
        scos.progress(16, h - 24, math.max(0, w - 32), 12,
                      math.floor(math.min(at, total) * 100 / total))
    end
end

return {
    open = function()
        scos.title("Isolation test")
        scos.interval(100)
        if not read_file(CURSOR) then
            write_file(CURSOR, "1")
            write_file(RESULTS, "")
            pcall(scos.remove, REPORT)
            pcall(scos.remove, "done.txt")
            pcall(scos.remove, "painted.txt")
        end
        step()
        scos.log("isolation: probing the sandbox boundary")
    end,
    tick = function()
        if step() then scos.redraw() end
        if not read_file("done.txt") and not read_file(CURSOR) then return end
        local at = tonumber(read_file(CURSOR) or "") or 1
        if at > #probes and not read_file("done.txt") then write_file("done.txt", "1") end
    end,
    paint = function(w, h)
        paint_summary(w, h)
        -- Last, because the budget probe deliberately spends what is left.
        run_paint_probes()
    end,
    key = function(code, down)
        if not down then return end
        if code == 114 or code == 82 then       -- R: run the probes again
            write_file(CURSOR, "1")
            write_file(RESULTS, "")
            pcall(scos.remove, REPORT)
            pcall(scos.remove, "done.txt")
            pcall(scos.remove, "painted.txt")
            step()
            scos.redraw()
        end
    end,
}
