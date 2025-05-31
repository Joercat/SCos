CC = gcc
CXX = g++
AS = nasm
LD = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -I./include -nostdlib -nostartfiles -nodefaultlibs -fno-stack-protector -fno-pic
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = -f elf32
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib

SRCDIR = .
OBJDIR = obj

SOURCES_C = 

SOURCES_CXX = kernel/main.cpp \
              ui/desktop.cpp \
              ui/window_manager.cpp \
              ui/app_launcher.cpp \
              apps/terminal.cpp \
              apps/shell.cpp \
              apps/notepad.cpp \
              apps/calendar.cpp \
              apps/settings.cpp \
              apps/about.cpp \
              fs/ramfs.cpp \
              drivers/keyboard.cpp \
              memory/heap.cpp \
              interrupt/idt.cpp \
              debug/serial.cpp \
              lib/string.cpp

SOURCES_ASM = bootloader.asm \
              interrupt/idt.asm

OBJECTS_C = $(SOURCES_C:%.c=$(OBJDIR)/%.o)
OBJECTS_CXX = $(SOURCES_CXX:%.cpp=$(OBJDIR)/%.o)
OBJECTS_ASM = $(SOURCES_ASM:%.asm=$(OBJDIR)/%.o)
OBJECTS = $(OBJECTS_C) $(OBJECTS_CXX) $(OBJECTS_ASM)

.PHONY: all clean

all: scos.bin

scos.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -rf $(OBJDIR) scos.bin

run: scos.bin
	qemu-system-i386 -kernel scos.bin -serial stdio

debug: scos.bin
	qemu-system-i386 -kernel scos.bin -serial stdio -s -S
