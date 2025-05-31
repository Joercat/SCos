This commit modifies the Makefile to include the mouse driver in the build process.
</replit_final_file>
```makefile
# SCos Makefile
CC = gcc
AS = nasm
LD = ld

# Compiler flags
CFLAGS = -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -c -I./include
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld

# Directories
SRCDIR = .
OBJDIR = obj
APPSDIR = apps
UIDIR = ui
KERNELDIR = kernel
LIBDIR = lib
DRIVERSDIR = drivers
FSDIR = fs
SECURITYDIR = security
DEBUGDIR = debug
INTERRUPTDIR = interrupt
MEMORYDIR = memory

# Source files
KERNEL_SOURCES = $(KERNELDIR)/main.cpp
APPS_SOURCES = $(APPSDIR)/terminal.cpp $(APPSDIR)/notepad.cpp $(APPSDIR)/calculator.cpp $(APPSDIR)/file_manager.cpp $(APPSDIR)/calendar.cpp $(APPSDIR)/settings.cpp $(APPSDIR)/about.cpp $(APPSDIR)/security_center.cpp $(APPSDIR)/shell.cpp $(APPSDIR)/browser.cpp $(APPSDIR)/app_store.cpp
UI_SOURCES = $(UIDIR)/window_manager.cpp $(UIDIR)/desktop.cpp $(UIDIR)/app_launcher.cpp
LIB_SOURCES = $(LIBDIR)/string.cpp
DRIVERS_SOURCES = $(DRIVERSDIR)/keyboard.cpp $(DRIVERSDIR)/mouse.cpp
FS_SOURCES = $(FSDIR)/ramfs.cpp
SECURITY_SOURCES = $(SECURITYDIR)/auth.cpp
DEBUG_SOURCES = $(DEBUGDIR)/serial.cpp
INTERRUPT_SOURCES = $(INTERRUPTDIR)/idt.cpp
MEMORY_SOURCES = $(MEMORYDIR)/heap.cpp

# Object files
KERNEL_OBJECTS = $(OBJDIR)/$(KERNELDIR)/main.o
APPS_OBJECTS = $(OBJDIR)/$(APPSDIR)/terminal.o $(OBJDIR)/$(APPSDIR)/notepad.o $(OBJDIR)/$(APPSDIR)/calculator.o $(OBJDIR)/$(APPSDIR)/file_manager.o $(OBJDIR)/$(APPSDIR)/calendar.o $(OBJDIR)/$(APPSDIR)/settings.o $(OBJDIR)/$(APPSDIR)/about.o $(OBJDIR)/$(APPSDIR)/security_center.o $(OBJDIR)/$(APPSDIR)/shell.o $(OBJDIR)/$(APPSDIR)/browser.o $(OBJDIR)/$(APPSDIR)/app_store.o
UI_OBJECTS = $(OBJDIR)/$(UIDIR)/window_manager.o $(OBJDIR)/$(UIDIR)/desktop.o $(OBJDIR)/$(UIDIR)/app_launcher.o
LIB_OBJECTS = $(OBJDIR)/$(LIBDIR)/string.o
DRIVERS_OBJECTS = $(OBJDIR)/$(DRIVERSDIR)/keyboard.o $(OBJDIR)/$(DRIVERSDIR)/mouse.o
FS_OBJECTS = $(OBJDIR)/$(FSDIR)/ramfs.o
SECURITY_OBJECTS = $(OBJDIR)/$(SECURITYDIR)/auth.o
DEBUG_OBJECTS = $(OBJDIR)/$(DEBUGDIR)/serial.o
INTERRUPT_OBJECTS = $(OBJDIR)/$(INTERRUPTDIR)/idt.o
MEMORY_OBJECTS = $(OBJDIR)/$(MEMORYDIR)/heap.o

# Assembly files
BOOTLOADER_ASM = bootloader.asm
IDT_ASM = $(INTERRUPTDIR)/idt.asm

ALL_OBJECTS = $(KERNEL_OBJECTS) $(APPS_OBJECTS) $(UI_OBJECTS) $(LIB_OBJECTS) $(DRIVERS_OBJECTS) $(FS_OBJECTS) $(SECURITY_OBJECTS) $(DEBUG_OBJECTS) $(INTERRUPT_OBJECTS) $(MEMORY_OBJECTS) $(OBJDIR)/bootloader.o $(OBJDIR)/$(INTERRUPTDIR)/idt_asm.o

# Targets
all: scos.bin

scos.bin: $(ALL_OBJECTS)
	$(LD) $(LDFLAGS) -o scos.bin $(ALL_OBJECTS)

# Bootloader
$(OBJDIR)/bootloader.o: $(BOOTLOADER_ASM)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

# IDT Assembly
$(OBJDIR)/$(INTERRUPTDIR)/idt_asm.o: $(IDT_ASM)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

# Kernel objects
$(OBJDIR)/$(KERNELDIR)/%.o: $(KERNELDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Apps objects
$(OBJDIR)/$(APPSDIR)/%.o: $(APPSDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# UI objects
$(OBJDIR)/$(UIDIR)/%.o: $(UIDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Library objects
$(OBJDIR)/$(LIBDIR)/%.o: $(LIBDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Drivers objects
$(OBJDIR)/$(DRIVERSDIR)/%.o: $(DRIVERSDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Filesystem objects
$(OBJDIR)/$(FSDIR)/%.o: $(FSDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Security objects
$(OBJDIR)/$(SECURITYDIR)/%.o: $(SECURITYDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Debug objects
$(OBJDIR)/$(DEBUGDIR)/%.o: $(DEBUGDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Interrupt objects
$(OBJDIR)/$(INTERRUPTDIR)/%.o: $(INTERRUPTDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Memory objects
$(OBJDIR)/$(MEMORYDIR)/%.o: $(MEMORYDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OBJDIR)
	rm -f scos.bin

run: scos.bin
	qemu-system-i386 -fda scos.bin

.PHONY: all clean run