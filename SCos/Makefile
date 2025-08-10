CC = gcc
ASM = nasm
CFLAGS = -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie -fno-exceptions -fno-rtti -fno-unwind-tables
INCLUDES = -I./include
LDFLAGS = -m elf_i386 -T linker.ld

KERNEL_OBJS = obj/kernel/main.o
APP_OBJS = obj/apps/terminal.o obj/apps/notepad.o obj/apps/calculator.o obj/apps/file_manager.o obj/apps/calendar.o obj/apps/settings.o obj/apps/about.o obj/apps/app_store.o obj/apps/security_center.o obj/apps/browser.o obj/apps/shell.o obj/apps/updates.o obj/apps/network_settings.o obj/apps/terminal_wrapper.o obj/apps/html_interpreter.o
UI_OBJS = obj/ui/desktop.o obj/ui/window_manager.o obj/ui/app_launcher.o obj/ui/theme_manager.o obj/ui/vga_utils.o
DRIVER_OBJS = obj/drivers/keyboard.o obj/drivers/mouse.o obj/drivers/network.o obj/drivers/bluetooth.o
LIB_OBJS = obj/lib/string.o
SECURITY_OBJS = obj/security/auth.o
FS_OBJS = obj/fs/ramfs.o
DEBUG_OBJS = obj/debug/serial.o
MEMORY_OBJS = obj/memory/heap.o
INTERRUPT_OBJS = obj/interrupt/idt.o obj/interrupt/idt_asm.o

ALL_OBJS = $(KERNEL_OBJS) $(APP_OBJS) $(UI_OBJS) $(DRIVER_OBJS) $(LIB_OBJS) $(SECURITY_OBJS) $(FS_OBJS) $(DEBUG_OBJS) $(MEMORY_OBJS) $(INTERRUPT_OBJS)

$(shell mkdir -p obj/kernel obj/apps obj/ui obj/drivers obj/lib obj/security obj/fs obj/debug obj/memory obj/interrupt)

.PHONY: all clean run

all: scos.img

scos.img: bootloader.bin kernel.bin
	dd if=/dev/zero of=scos.img bs=512 count=2880
	dd if=bootloader.bin of=scos.img bs=512 count=1 conv=notrunc
	dd if=kernel.bin of=scos.img bs=512 seek=1 conv=notrunc
	@echo "Disk image created: scos.img"
	@echo "Image size: $$(stat -c%s scos.img) bytes"

bootloader.bin: bootloader.asm
	$(ASM) -f bin bootloader.asm -o bootloader.bin

kernel.bin: $(ALL_OBJS)
	ld $(LDFLAGS) -o kernel.bin $(ALL_OBJS)

obj/kernel/%.o: kernel/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/apps/%.o: apps/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/ui/%.o: ui/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/drivers/%.o: drivers/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/lib/%.o: lib/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/security/%.o: security/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/fs/%.o: fs/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/debug/%.o: debug/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/memory/%.o: memory/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/interrupt/idt.o: interrupt/idt.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c interrupt/idt.cpp -o obj/interrupt/idt.o

obj/interrupt/idt_asm.o: interrupt/idt.asm
	$(ASM) -f elf32 interrupt/idt.asm -o obj/interrupt/idt_asm.o

clean:
	rm -rf obj
	rm -f *.bin *.img kernel_padded.bin *.log

run: all
	pkill -f "qemu-system-i386" || true
	sleep 1
	qemu-system-i386 -drive format=raw,file=scos.img -m 32M -no-reboot -no-shutdown -vga std

run-headless: scos.img
	qemu-system-i386 -drive format=raw,file=scos.img -m 32M -serial stdio -no-reboot -no-shutdown -nographic

run-simple: scos.img
	qemu-system-i386 -drive format=raw,file=scos.img -m 32M -serial stdio

debug: scos.img
	qemu-system-i386 -drive format=raw,file=scos.img -m 32M -serial stdio -d int,cpu_reset -no-reboot -no-shutdown

run-debug: scos.img
	qemu-system-i386 -drive format=raw,file=scos.img -m 32M -serial stdio -no-reboot -no-shutdown -nographic
