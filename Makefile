# Active SCos target: freestanding x86-64 startup foundation, no release number.
CC := gcc
LD := ld
OBJCOPY := objcopy
BUILD := build
CFLAGS := -m64 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only \
          -mcmodel=small -O2 -Wall -Wextra -Werror -std=c11 -Ikernel/include
SOURCES := $(wildcard kernel/src/*.c)
OBJECTS := $(patsubst kernel/src/%.c,$(BUILD)/%.o,$(SOURCES))
.PHONY: all clean legacy milestone-check
all: milestone-check $(BUILD)/scos.img
milestone-check:
	python3 tools/check_milestone.py
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: kernel/src/%.c kernel/include/kernel.h kernel/include/boot.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/entry.o: kernel/x86_64/entry.S | $(BUILD)
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/vectors.S: tools/gen_vectors.py | $(BUILD)
	python3 $< > $@
$(BUILD)/vectors.o: $(BUILD)/vectors.S
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
$(BUILD)/kernel.elf: $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o kernel/x86_64/linker.ld
	$(LD) -m elf_x86_64 --build-id=none -z noexecstack -z max-page-size=0x1000 \
	      -T kernel/x86_64/linker.ld -o $@ $(BUILD)/entry.o $(OBJECTS) $(BUILD)/vectors.o
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
$(BUILD)/stage1.bin: boot/stage1.S | $(BUILD)
	$(CC) -m32 -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -Wl,--oformat,binary -Wl,-Ttext,0x7c00 $< -o $@
$(BUILD)/stage2.elf: boot/stage2.S | $(BUILD)
	$(CC) -m32 -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -Wl,--oformat=elf32-i386 -Wl,-Ttext,0x8000 $< -o $@
$(BUILD)/stage2.bin: $(BUILD)/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@
$(BUILD)/scos.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/stage2.elf $(BUILD)/kernel.bin $(BUILD)/kernel.elf tools/makedisk.py
	python3 tools/makedisk.py $(BUILD)
legacy:
	$(MAKE) -C legacy/i386
clean:
	rm -rf $(BUILD)
