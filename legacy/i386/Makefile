# SCos native - build system
#
# Targets:
#   make            - build build/scos.img (bootable disk image)
#   make font       - regenerate the bitmap font
#   make clean

CC      := gcc
OBJCOPY := objcopy
PYTHON  := python3

KERN_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
               -mno-sse -mno-mmx -fno-asynchronous-unwind-tables \
               -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
               -Ikernel/include

BUILD := build

KERN_SRC := $(wildcard kernel/src/*.c)
KERN_OBJ := $(patsubst kernel/src/%.c,$(BUILD)/kobj/%.o,$(KERN_SRC))

.PHONY: all font clean milestone-check

all: milestone-check $(BUILD)/scos.img

milestone-check:
	python3 ../../tools/check_milestone.py

$(BUILD)/kobj:
	@mkdir -p $(BUILD)/kobj

$(BUILD)/kobj/%.o: kernel/src/%.c kernel/include/scos.h | $(BUILD)/kobj
	$(CC) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/kobj/entry.o: kernel/entry.S | $(BUILD)/kobj
	$(CC) -m32 -ffreestanding -c $< -o $@

$(BUILD)/kernel.elf: $(KERN_OBJ) $(BUILD)/kobj/entry.o kernel/linker.ld
	$(CC) -m32 -no-pie -ffreestanding -nostdlib -T kernel/linker.ld \
	      -Wl,--build-id=none -o $@ $(BUILD)/kobj/entry.o $(KERN_OBJ)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

$(BUILD)/stage1.bin: boot/stage1.S
	@mkdir -p $(BUILD)
	$(CC) -m32 -ffreestanding -nostdlib -no-pie -Wl,--oformat,binary -Wl,-Ttext,0x7c00 $< -o $@

$(BUILD)/stage2.elf: boot/stage2.S
	@mkdir -p $(BUILD)
	$(CC) -m32 -ffreestanding -nostdlib -no-pie -Wl,--oformat=elf32-i386 -Wl,-Ttext,0x8000 $< -o $@

$(BUILD)/stage2.bin: $(BUILD)/stage2.elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

$(BUILD)/scos.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/makedisk.py $(BUILD)/stage2.elf
	$(PYTHON) tools/makedisk.py $(BUILD)

font:
	$(PYTHON) ../../tools/fontgen.py kernel/src/font_data.c

# Changes to the framebuffer range planner must rebuild its consumer.
$(BUILD)/kobj/fb.o: kernel/include/mtrr_plan.h

clean:
	rm -rf $(BUILD)
