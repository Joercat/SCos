# Native x64 UEFI application + freestanding AMD64 kernel. No BIOS target.
CC := gcc
LD := ld
BUILD := build
COMMON := -m64 -ffreestanding -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only -O2 -Wall -Wextra -Werror -std=c11 -Ikernel/include
CFLAGS := $(COMMON) -fpie -mcmodel=small
EFI_CFLAGS := $(COMMON) -fpie -fno-ident -fshort-wchar -Iboot/uefi
SOURCES := $(wildcard kernel/src/*.c)
OBJECTS := $(patsubst kernel/src/%.c,$(BUILD)/%.o,$(SOURCES))
DESKTOP_SOURCES := $(wildcard kernel/desktop/*.c)
DESKTOP_OBJECTS := $(patsubst kernel/desktop/%.c,$(BUILD)/desktop-%.o,$(DESKTOP_SOURCES))
DRIVER_SOURCES := $(wildcard kernel/drivers/*.c)
DRIVER_OBJECTS := $(patsubst kernel/drivers/%.c,$(BUILD)/driver-%.o,$(DRIVER_SOURCES))
# Per-family GPU modules live in their own directory so a family's table, port state
# and (later) engine code are separate units: detection names a chip without any of the
# engine code running, and a ported family is added per file rather than in one blob.
GPU_SOURCES := $(wildcard kernel/drivers/gpu/*.c)
GPU_OBJECTS := $(patsubst kernel/drivers/gpu/%.c,$(BUILD)/gpu-%.o,$(GPU_SOURCES))
.PHONY: all clean milestone-check gpu-modules
all: milestone-check $(BUILD)/scos.img
milestone-check:
	python3 tools/check_milestone.py
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: kernel/src/%.c kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/entry.o: kernel/x86_64/entry.S | $(BUILD)
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/vectors.S: tools/gen_vectors.py | $(BUILD)
	python3 $< > $@
$(BUILD)/vectors.o: $(BUILD)/vectors.S
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/kernel.elf: $(GPU_OBJECTS) $(DRIVER_OBJECTS) $(DESKTOP_OBJECTS) $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o kernel/x86_64/linker.ld Makefile
	$(LD) -m elf_x86_64 -pie --no-dynamic-linker -Bsymbolic --build-id=none -z noexecstack -z max-page-size=0x1000 -T kernel/x86_64/linker.ld -o $@ $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o $(DESKTOP_OBJECTS) $(DRIVER_OBJECTS) $(GPU_OBJECTS) $(BUILD)/lua-runtime.o
$(BUILD)/efi-main.o: boot/uefi/main.c boot/uefi/efi.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@
$(BUILD)/efi-font.o: kernel/src/font.c Makefile | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@
# The boot stub names a GPU family to decide which single module file to read, so it shares the
# matcher and the generated tables with the kernel instead of carrying its own copy of the rules.
$(BUILD)/efi-gpu_match.o: kernel/drivers/gpu/gpu_match.c kernel/drivers/gpu/gpu_ids.h \
                          kernel/include/gpu_match.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@
$(BUILD)/BOOTX64.EFI: $(BUILD)/efi-main.o $(BUILD)/efi-font.o $(BUILD)/efi-gpu_match.o
	$(LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0 --no-insert-timestamp --enable-reloc-section -o $@ $(BUILD)/efi-main.o $(BUILD)/efi-font.o $(BUILD)/efi-gpu_match.o
$(BUILD)/scos.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.elf tools/makedisk.py $(GPU_MODULE_FILES)
	python3 tools/makedisk.py $(BUILD)

# ---------------------------------------------------------- GPU driver modules ----
# One loadable display module per GPU family, from drivers/gpu/<family>/.  These are separate
# files on the boot disk, and the boot stub opens only the module whose family the shared matcher
# names for the detected chip: an unneeded family costs no RAM, no page tables and no CPU time,
# and each port stays independently editable.
MODULE_DIR := $(BUILD)/gpu
MODULE_FAMILIES := $(notdir $(wildcard drivers/gpu/*))
MODULE_CFLAGS := $(COMMON) -fno-pie -mcmodel=small -fno-strict-aliasing
define MODULE_rules
MODULE_SRCS_$1 := $$(wildcard drivers/gpu/$1/*.c)
MODULE_OBJS_$1 := $$(patsubst drivers/gpu/$1/%.c,$$(BUILD)/gmod-$1-%.o,$$(MODULE_SRCS_$1))
$$(BUILD)/gmod-$1-%.o: drivers/gpu/$1/%.c $$(wildcard drivers/gpu/$1/*.h drivers/gpu/$1/*.inc) \
                       kernel/include/gpu_abi.h Makefile | $$(BUILD)
	$$(CC) $$(MODULE_CFLAGS) -Idrivers/gpu/$1 -c $$< -o $$@
$$(MODULE_DIR)/$1.o: $$(MODULE_OBJS_$1) | $$(MODULE_DIR)
	$$(LD) -r -m elf_x86_64 -o $$@ $$^
$$(MODULE_DIR)/$1.mod: $$(MODULE_DIR)/$1.o tools/build_gpu_module.py
	python3 tools/build_gpu_module.py --input $$< --family $1 --out $$@ --report
GPU_MODULE_FILES += $$(MODULE_DIR)/$1.mod
endef
$(foreach family,$(MODULE_FAMILIES),$(eval $(call MODULE_rules,$(family))))

$(MODULE_DIR):
	mkdir -p $@
gpu-modules: $(GPU_MODULE_FILES)
	python3 tools/build_gpu_module.py --verify
clean:
	rm -rf $(BUILD)

APP_CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mno-avx -fno-strict-aliasing
$(BUILD)/desktop-%.o: kernel/desktop/%.c kernel/include/scos.h kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@
# Every callback reachable from a hardware IRQ remains general-register-only.
$(BUILD)/desktop-kbd.o $(BUILD)/desktop-mouse.o $(BUILD)/desktop-cpumeter.o $(BUILD)/desktop-klog.o $(BUILD)/desktop-platform.o: APP_CFLAGS = $(CFLAGS) -fno-strict-aliasing
$(BUILD)/driver-%.o: kernel/drivers/%.c kernel/include/scos.h kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -fno-strict-aliasing -c $< -o $@
$(BUILD)/gpu-%.o: kernel/drivers/gpu/%.c kernel/drivers/gpu/gpu_ids.h kernel/include/gpu.h kernel/include/scos.h kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -fno-strict-aliasing -c $< -o $@

include kernel/lua/runtime.mk
