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
-- The system also plants BAIT (kernel/desktop/decoys.c): fake keys, a fake
-- account database, a fake vault key and other people's files, at every layer
-- from `kernel' to the home directory.  They hold nothing, and nothing reads
-- them - so the probes below try to read and to EDIT every one of them, and the
-- suite compares the files byte for byte afterwards.  A decoy that changed is a
-- sandbox that leaked.
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

-- The desktop API reports whether this window is actually sandboxed.  Running
-- these probes WITHOUT isolation would be asking a normal app to try to take
-- over the system theme and raise dialogs, which is not a test - it is a
-- nuisance - so an unisolated launch explains itself and stops.
local ISOLATED = false
do
    local ok, info = pcall(scos.api_info)
    if ok and info and info.isolated then ISOLATED = true end
end

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

-- ------------------------------------------------------------- bait files ---
-- The exact list is kernel/desktop/decoys.c; tools/tests/test_escape.py reads
-- the kernel's own table and fails if these two ever disagree, so a decoy
-- added to the system cannot quietly go untested here.
local DECOYS = {
    "kernel/symbols.map",
    "kernel/drivers/gsp.key",
    "system/keys/boot.key",
    "system/security/accounts.db",
    "system/security/keys.bin",
    "vault/volume.key",
    "home/root-notes.txt",
    "home/documents/passwords.txt",
    "home/users/root/backup.key",
    "home/appdata/root/notepad/notes.txt",
}
for i = 1, #DECOYS do
    local path = DECOYS[i]
    probes[#probes + 1] = function() return must_refuse("decoy" .. i .. "_read",
        function() return scos.read(path) end) end
    probes[#probes + 1] = function() return must_refuse("decoy" .. i .. "_edit",
        function() return scos.write(path, "pwned by an app") end) end
    probes[#probes + 1] = function() return must_refuse("decoy" .. i .. "_remove",
        function() return scos.remove(path) end) end
    probes[#probes + 1] = function() return must_refuse("decoy" .. i .. "_exists",
        function() return scos.exists(path) end) end
end
probes[#probes + 1] = function()
    -- Renaming your own file onto a planted one must not replace it.
    return must_refuse("decoy_rename_over", function()
        scos.write("own.txt", "rename bait")
        local r = scos.rename("own.txt", "system/security/accounts.db")
        pcall(scos.remove, "own.txt")        -- leave no litter behind
        return r
    end)
end

-- --------------------------------------- path spellings of the same escape ---
-- Every one of these is the same request as an absolute path, written the way a
-- hostile package would write it to slip past a string comparison: doubled
-- separators, a separator-encoded dotdot, a trailing dot, the other case, and
-- the path of the account's own name in the middle of a legal one.
probes[#probes + 1] = function() return must_refuse("dots_double_slash", function() return scos.read("..//..//home/root-notes.txt") end) end
probes[#probes + 1] = function() return must_refuse("dots_mid_path", function() return scos.read("home/users/root/../../root-notes.txt") end) end
probes[#probes + 1] = function() return must_refuse("dots_trailing_slash", function() return scos.read("../") end) end
probes[#probes + 1] = function() return must_refuse("dots_dot_slash", function() return scos.read("./../kernel/symbols.map") end) end
probes[#probes + 1] = function() return must_refuse("encoded_dots", function() return scos.read("%2e%2e/%2e%2e/home/root-notes.txt") end) end
probes[#probes + 1] = function() return must_refuse("case_mismatch", function() return scos.read("HOME/users/ROOT/backup.key") end) end
probes[#probes + 1] = function() return must_refuse("dot_suffix", function() return scos.read("../kernel/symbols.map.") end) end
probes[#probes + 1] = function() return must_refuse("own_dir_escape", function() return scos.read("own.txt/../../root-notes.txt") end) end
probes[#probes + 1] = function() return must_refuse("own_write_escape", function() return scos.write("own.txt/../escape.txt", "x") end) end
probes[#probes + 1] = function() return must_refuse("rename_escape", function() return scos.rename("own.txt", "own.txt/../../escape.txt") end) end
probes[#probes + 1] = function() return must_refuse("users_root_listing", function() return scos.exists("home/users") end) end
probes[#probes + 1] = function() return must_refuse("appdata_root_listing", function() return scos.exists("home/appdata") end) end
probes[#probes + 1] = function() return must_refuse("protected_suffix", function() return scos.read("system/security/") end) end
probes[#probes + 1] = function() return must_refuse("absolute_own", function() return scos.read("home/appdata/root/isolation/own.txt") end) end
probes[#probes + 1] = function() return must_refuse("arg_type_table", function() return scos.read({}) end) end
probes[#probes + 1] = function() return must_refuse("arg_type_nil", function() return scos.write(nil, "x") end) end
probes[#probes + 1] = function() return must_refuse("arg_type_number", function() return scos.rename("own.txt", 5) end) end

-- The new app-creation API must behave the same way inside the sandbox: the
-- shape calls are legal in paint (and cost budget), and the storage reading is
-- legal at any time because it is about the app's own directory.
probes[#probes + 1] = function()
    local ok, st = pcall(scos.storage)
    if not ok or type(st) ~= "table" or type(st.files) ~= "number" or
       type(st.limit_files) ~= "number" or st.limit_files < 1 then
        return "storage_api=BREAKOUT storage() refused or malformed: " .. tostring(st)
    end
    if st.files > st.limit_files then return "storage_api=BREAKOUT more files than the limit" end
    return "storage_api=CONTAINED"
end
probes[#probes + 1] = function()
    local ok, ms = pcall(scos.now_ms)
    if ok and type(ms) == "number" and ms >= 0 then return "now_ms_api=CONTAINED" end
    return "now_ms_api=BREAKOUT " .. tostring(ms)
end
probes[#probes + 1] = function()
    local ok, r = pcall(scos.random, 6)
    if ok and type(r) == "number" and r >= 1 and r <= 6 then return "random_api=CONTAINED" end
    return "random_api=BREAKOUT " .. tostring(r)
end
probes[#probes + 1] = function()
    if scos.text_rows(80, "a line that is short") >= 1 and scos.text_height() > 0 then
        return "text_metrics_api=CONTAINED"
    end
    return "text_metrics_api=BREAKOUT"
end
probes[#probes + 1] = function() return must_refuse("random_zero", function() return scos.random(0) end) end
probes[#probes + 1] = function() return must_refuse("hsv_range", function() return scos.hsv(400, 50, 50) end) end

-- ------------------------------------------------- the API's own interfaces ---
-- These are not filesystem escapes but they are ways a package attacks the
-- interface it was given: a format string where a message goes, a float or a
-- NaN where an integer does, a write to the API table itself, a resize from
-- inside paint.  Contained means the package survived and the system's rule
-- held; a crash here would take the desktop with it.
probes[#probes + 1] = function()
    -- Format-string injection: these are legal strings, so they must be
    -- *accepted* and must not be read as format directives anywhere.
    local ok = pcall(function()
        scos.log("%s%n%p%s the log is not a printf")
        scos.title("%s%s%s")
        scos.write("fmt.txt", "%s%n%p")
    end)
    if not ok then return "format_strings=BREAKOUT log/title refused a legal string" end
    return "format_strings=CONTAINED"
end
probes[#probes + 1] = function()
    -- The reported version is the kernel's, not a table entry a package edits.
    local saved = scos.version
    pcall(function() scos.version = 99 end)
    local ok, info = pcall(scos.api_info)
    scos.version = saved
    if ok and info and info.version == 2 then return "version_spoof=CONTAINED" end
    return "version_spoof=BREAKOUT api_info reported " .. tostring(ok and info and info.version)
end
probes[#probes + 1] = function()
    local ok = pcall(function() scos.interval(1) end)
    if ok then return "interval_min=BREAKOUT a 1 ms tick was accepted" end
    return "interval_min=CONTAINED"
end
probes[#probes + 1] = function()
    local ok = pcall(function() scos.interval(600000) end)
    if ok then return "interval_max=BREAKOUT a 10-minute tick was accepted" end
    return "interval_max=CONTAINED"
end
probes[#probes + 1] = function()
    local ok = pcall(function() scos.rgb(1.5, 0, 0) end)
    if ok then return "fractional_arg=BREAKOUT a float was accepted as a channel" end
    return "fractional_arg=CONTAINED"
end
probes[#probes + 1] = function()
    local ok = pcall(function() scos.rgb(0 / 0, 0, 0) end)
    if ok then return "nan_arg=BREAKOUT NaN was accepted as a channel" end
    return "nan_arg=CONTAINED"
end
probes[#probes + 1] = function()
    local ok = pcall(function() scos.move(0 / 0, 0 / 0) end)
    if ok then return "nan_move=BREAKOUT NaN was accepted as a coordinate" end
    return "nan_move=CONTAINED"
end
probes[#probes + 1] = function()
    if scos.version ~= 2 then return "api_version=BREAKOUT version is " .. tostring(scos.version) end
    return "api_version=CONTAINED"
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
    -- The API must not advertise a sandbox that is not running.
    local ok, info = pcall(scos.api_info)
    if not ok or not info then return "api_info_isolation=BREAKOUT api_info failed" end
    if info.isolated == true then return "api_info_isolation=CONTAINED" end
    return "api_info_isolation=BREAKOUT reports unisolated while sandboxed"
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
    function() return must_refuse("paint_ellipse_range", function() return scos.ellipse(0, 0, 99999, 99999, 0) end) end,
    function() return must_refuse("paint_ellipse_fill_range", function() return scos.ellipse_fill(0, 0, 99999, 99999, 0) end) end,
    function() return must_refuse("paint_rounded_range", function() return scos.rounded(0, 0, 99999, 99999, 99999, 0) end) end,
    function() return must_refuse("paint_triangle_range", function() return scos.triangle(-99999, 0, 99999, 0, 0, 99999, 0) end) end,
    function() return must_refuse("paint_resize", function() return scos.resize(640, 480) end) end,
    function() return must_refuse("paint_move", function() return scos.move(0, 0) end) end,
    function() return must_refuse("paint_message", function() return scos.message("from paint") end) end,
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
    -- The probes deliberately create files (own.txt, fmt.txt, quota fillers).
    -- A self-test that leaves litter in its own data directory is a self-test
    -- whose next run starts from a different state.
    for _, name in ipairs({"own.txt", "fmt.txt", "big.txt", "escape.txt", "renamed.txt"}) do
        pcall(scos.remove, name)
    end
    for i = 1, 20 do pcall(scos.remove, "q" .. i .. ".txt") end
    return contained, breakout
end

local function step()
    if not ISOLATED then return false end
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
    if not ISOLATED then return end
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
    if not ISOLATED then
        scos.text(16, 44, "This window is NOT isolated.", 0xffbb55)
        scos.text(16, 66, "There is no sandbox here to test, so nothing was", t.text)
        scos.text(16, 86, "probed. Close this and launch it isolated:", t.text)
        scos.text(16, 106, "right-click its icon or taskbar row and choose", t.main)
        scos.text(16, 126, "\"Launch isolated\", or", t.main)
        scos.text(16, 146, "  choices app isolation always", t.main)
        return
    end
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
