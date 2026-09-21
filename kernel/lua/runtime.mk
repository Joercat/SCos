LUA_NAMES := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib ltablib lstrlib lutf8lib lmathlib
LUA_OBJS := $(addprefix $(BUILD)/lua-,$(addsuffix .o,$(LUA_NAMES)))
LUA_FLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mno-avx -fno-strict-aliasing -ffunction-sections -fdata-sections -Wno-unused-but-set-variable -Wno-parentheses -Wno-sign-compare -DSCOS_LUA -Ithird_party/lua -Ithird_party/stb -Ikernel/lua -include kernel/lua/port.h
MUSL_SRC := $(wildcard third_party/musl/math/*.c) third_party/musl/internal/floatscan.c
MUSL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(MUSL_SRC))
$(BUILD)/lua-%.o: third_party/lua/%.c $(wildcard third_party/lua/*.h) kernel/lua/port.h kernel/lua/runtime.mk Makefile | $(BUILD)
	$(CC) $(LUA_FLAGS) -Wno-unused-function -c $< -o $@
$(BUILD)/third_party/musl/%.o: third_party/musl/%.c kernel/lua/port.h kernel/lua/runtime.mk Makefile $(wildcard third_party/musl/internal/*.h third_party/musl/math/*.h)
	mkdir -p $(dir $@)
	$(CC) $(LUA_FLAGS) -Dhidden= -Ithird_party/musl/internal -Ithird_party/musl/math -c $< -o $@
$(BUILD)/lua-libc.o: kernel/lua/libc.c third_party/stb/stb_sprintf.h third_party/musl/internal/scan.h kernel/lua/port.h kernel/lua/runtime.mk Makefile | $(BUILD)
	$(CC) $(LUA_FLAGS) -Ithird_party/musl/internal -c $< -o $@
$(BUILD)/lua-jump.o: kernel/lua/jump.S | $(BUILD)
	$(CC) -m64 -c $< -o $@
$(BUILD)/lua-runtime.o: $(LUA_OBJS) $(MUSL_OBJS) $(BUILD)/lua-libc.o $(BUILD)/lua-jump.o $(BUILD)/desktop-lua_apps.o
	$(LD) -r --gc-sections $$(nm -u $(BUILD)/desktop-lua_apps.o | awk '/ U lua/{print "-u " $$2}') -o $@ $(LUA_OBJS) $(MUSL_OBJS) $(BUILD)/lua-libc.o $(BUILD)/lua-jump.o
$(BUILD)/kernel.elf: $(BUILD)/lua-runtime.o
$(BUILD)/desktop-lua_apps.o: APP_CFLAGS += -Ithird_party/lua
$(BUILD)/lua_examples.h: tools/embed_apps.py third_party/NOTICES.txt $(wildcard apps/examples/*.lua) | $(BUILD)
	python3 tools/embed_apps.py $@
$(BUILD)/desktop-vfs.o: $(BUILD)/lua_examples.h
$(BUILD)/desktop-vfs.o: APP_CFLAGS += -I$(BUILD)

$(BUILD)/desktop-lua_apps.o: $(wildcard third_party/lua/*.h)

$(BUILD)/desktop-lua_apps.o: kernel/desktop/lua_api.inc kernel/include/cat.h
$(BUILD)/desktop-cat.o $(BUILD)/desktop-app_studio.o $(BUILD)/desktop-apps.o: kernel/include/cat.h
