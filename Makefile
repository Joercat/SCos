# SCos native - build system
#
# Targets:
#   make            - build build/scos.img (bootable disk image)
#   make font       - regenerate the bitmap font
#   make test       - run the headless v86 verification suite
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

.PHONY: all font test clean

all: $(BUILD)/scos.img

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

$(BUILD)/scos.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin $(BUILD)/stage2.elf
	$(PYTHON) tools/makedisk.py $(BUILD)

font:
	$(PYTHON) tools/fontgen.py kernel/src/font_data.c

test: all usbtest
	node tests/run_tests.mjs

# native USB-logic simulator: runs the REAL usb.c against a mini xHC and
# the field-captured descriptors of the user's actual devices (see the
# header of tests/usb_sim.c).  Catches ring/cycle/parser regressions
# without a flash-and-boot cycle.
usbtest: build/usb_sim
	./build/usb_sim

build/usb_sim: tests/usb_sim.c kernel/src/usb.c kernel/src/mouse.c \
               kernel/src/kbd.c kernel/src/acpi.c kernel/include/scos.h
	@mkdir -p build
	gcc -std=gnu11 -no-pie -Wall -Wextra -Wno-unused-parameter \
	    -Wno-pointer-to-int-cast -Wno-unused-but-set-variable \
	    -Ikernel/include -o $@ tests/usb_sim.c

vendor:
	./tools/setup_preview.sh

# live browser preview: http://localhost:8080 (override with PORT=...)
preview: all vendor
	node tools/preview_server.mjs $(or $(PORT),8080)

clean:
	rm -rf $(BUILD)
