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
.PHONY: all clean milestone-check
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
$(BUILD)/kernel.elf: $(DRIVER_OBJECTS) $(DESKTOP_OBJECTS) $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o kernel/x86_64/linker.ld Makefile
	$(LD) -m elf_x86_64 -pie --no-dynamic-linker -Bsymbolic --build-id=none -z noexecstack -z max-page-size=0x1000 -T kernel/x86_64/linker.ld -o $@ $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o $(DESKTOP_OBJECTS) $(DRIVER_OBJECTS) $(BUILD)/lua-runtime.o
$(BUILD)/efi-main.o: boot/uefi/main.c boot/uefi/efi.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@
$(BUILD)/efi-font.o: kernel/src/font.c Makefile | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@
$(BUILD)/BOOTX64.EFI: $(BUILD)/efi-main.o $(BUILD)/efi-font.o
	$(LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0 --no-insert-timestamp --enable-reloc-section -o $@ $(BUILD)/efi-main.o $(BUILD)/efi-font.o
$(BUILD)/scos.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.elf tools/makedisk.py
	python3 tools/makedisk.py $(BUILD)
clean:
	rm -rf $(BUILD)

APP_CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mno-avx -fno-strict-aliasing
$(BUILD)/desktop-%.o: kernel/desktop/%.c kernel/include/scos.h kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@
# Every callback reachable from a hardware IRQ remains general-register-only.
$(BUILD)/desktop-kbd.o $(BUILD)/desktop-mouse.o $(BUILD)/desktop-cpumeter.o $(BUILD)/desktop-klog.o $(BUILD)/desktop-platform.o: APP_CFLAGS = $(CFLAGS) -fno-strict-aliasing
$(BUILD)/driver-%.o: kernel/drivers/%.c kernel/include/scos.h kernel/include/kernel.h kernel/include/boot.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -fno-strict-aliasing -c $< -o $@

include kernel/lua/runtime.mk
