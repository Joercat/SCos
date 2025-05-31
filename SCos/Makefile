# SCos Makefile
CC = gcc
ASM = nasm
CFLAGS = -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2
INCLUDES = -I./include
LDFLAGS = -m elf_i386 -T linker.ld

# Object files
KERNEL_OBJS = obj/kernel/main.o
APP_OBJS = obj/apps/terminal.o obj/apps/calculator.o obj/apps/notepad.o obj/apps/file_manager.o obj/apps/calendar.o obj/apps/settings.o obj/apps/about.o obj/apps/security_center.o obj/apps/browser.o obj/apps/app_store.o obj/apps/shell.o obj/apps/terminal_wrapper.o
UI_OBJS = obj/ui/desktop.o obj/ui/window_manager.o obj/ui/app_launcher.o obj/ui/theme_manager.o
DRIVER_OBJS = obj/drivers/keyboard.o obj/drivers/mouse.o
LIB_OBJS = obj/lib/string.o
SECURITY_OBJS = obj/security/auth.o
FS_OBJS = obj/fs/ramfs.o
DEBUG_OBJS = obj/debug/serial.o
MEMORY_OBJS = obj/memory/heap.o
INTERRUPT_OBJS = obj/interrupt/idt.o

ALL_OBJS = $(KERNEL_OBJS) $(APP_OBJS) $(UI_OBJS) $(DRIVER_OBJS) $(LIB_OBJS) $(SECURITY_OBJS) $(FS_OBJS) $(DEBUG_OBJS) $(MEMORY_OBJS) $(INTERRUPT_OBJS)

# Create object directories
$(shell mkdir -p obj/kernel obj/apps obj/ui obj/drivers obj/lib obj/security obj/fs obj/debug obj/memory obj/interrupt)

.PHONY: all clean run

all: scos.bin

scos.bin: bootloader.bin kernel.bin
	cat bootloader.bin kernel.bin > scos.bin

bootloader.bin: bootloader.asm
	$(ASM) -f bin bootloader.asm -o bootloader.bin

kernel.bin: $(ALL_OBJS)
	ld $(LDFLAGS) -o kernel.bin $(ALL_OBJS)

# Kernel objects
obj/kernel/%.o: kernel/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Application objects
obj/apps/%.o: apps/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# UI objects
obj/ui/%.o: ui/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Driver objects
obj/drivers/%.o: drivers/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Library objects
obj/lib/%.o: lib/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Security objects
obj/security/%.o: security/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Filesystem objects
obj/fs/%.o: fs/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Debug objects
obj/debug/%.o: debug/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Memory objects
obj/memory/%.o: memory/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Interrupt objects
obj/interrupt/%.o: interrupt/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

obj/interrupt/idt.o: interrupt/idt.asm
	$(ASM) -f elf32 interrupt/idt.asm -o obj/interrupt/idt.o

clean:
	rm -rf obj
	rm -f *.bin

run: scos.bin
	qemu-system-i386 -drive format=raw,file=scos.bin

# NOTE: To change the theme, modify the source files related to the UI (e.g., ui/theme_manager.cpp).
# Set the background color to black and the text/secondary color to #39ff14.
# Ensure that these changes are applied consistently across all UI elements.