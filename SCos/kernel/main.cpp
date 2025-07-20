#include "../ui/desktop.hpp"
#include "../fs/ramfs.hpp"
#include "../drivers/keyboard.hpp"
#include "../drivers/network.hpp"
#include "../drivers/bluetooth.hpp"
#include "../memory/heap.hpp"
#include "../interrupt/idt.hpp"
#include <stdint.h>
#include "../include/stddef.h"
#include "../include/stdarg.h"
#include "../debug/serial.hpp"
#include "../include/kernel.h"
#include "../include/memory.h"

extern "C" {
    extern void* __CTOR_LIST__;
    extern void* __CTOR_END__;
    extern uint32_t _kernel_end;

    typedef void (*constructor)();

    void call_constructors() {
        constructor* start = (constructor*)&__CTOR_LIST__;
        constructor* end = (constructor*)&__CTOR_END__;

        for (constructor* func = start; func < end; func++) {
            if (*func) {
                (*func)();
            }
        }
    }

    void kernel_panic(const char* message) {
        asm volatile("cli");
        serial_printf("KERNEL PANIC: %s\n", message);
        serial_printf("System halted.\n");
        while (1) {
            asm volatile("hlt");
        }
    }

    bool init_subsystems() {
        serial_printf("Initializing kernel subsystems...\n");

        // Skip serial init since we already did it
        serial_printf("Serial: OK\n");

        serial_printf("Initializing IDT...\n");
        if (!init_idt()) {
            serial_printf("IDT: FAILED\n");
            return false;
        }
        serial_printf("IDT: OK\n");

        serial_printf("Initializing Heap...\n");
        if (!init_heap()) {
            serial_printf("Heap: FAILED\n");
            return false;
        }
        serial_printf("Heap: OK\n");

        serial_printf("Initializing Keyboard...\n");
        if (!init_keyboard()) {
            serial_printf("Keyboard: FAILED\n");
            return false;
        }
        serial_printf("Keyboard: OK\n");

        serial_printf("Initializing Filesystem...\n");
        if (!initFS()) {
            serial_printf("Filesystem: FAILED\n");
            return false;
        }
        serial_printf("Filesystem: OK\n");

        serial_printf("Initializing Network Driver...\n");
        // Initialize network drivers
        NetworkDriver::init();
        serial_printf("Network Driver: OK\n");

        serial_printf("Initializing Bluetooth Driver...\n");
        // Initialize bluetooth
        BluetoothDriver::init();
        serial_printf("Bluetooth Driver: OK\n");

        serial_printf("All subsystems initialized successfully\n");
        return true;
    }
}

void show_memory_info() {
    serial_printf("=== MEMORY LAYOUT ===\n");
    serial_printf("Kernel loaded at: 0x1000\n");
    serial_printf("Kernel end: 0x%x\n", _kernel_end);
    serial_printf("Kernel size: %d KB\n", (_kernel_end - 0x1000) / 1024);
    serial_printf("Available memory starts at: 0x%x\n", _kernel_end);
    
    // Show QEMU's expected memory layout
    serial_printf("Expected QEMU layout:\n");
    serial_printf("  - Low memory: 0x0 - 0x9FFFF (640KB)\n");
    serial_printf("  - High memory: 0x100000+ (1MB+)\n");
    serial_printf("  - Our kernel: 0x1000 - 0x%x\n", _kernel_end);
    serial_printf("===================\n");
}

extern "C" void _start() __attribute__((section(".text._start")));

extern "C" void _start() {
    // Ensure we're in a known state first
    asm volatile("cli");
    
    // Clear the screen using proper 16-bit writes
    volatile uint16_t* video_memory = (volatile uint16_t*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) {
        video_memory[i] = 0x0720; // Space character with white on black
    }
    
    // Simple function to write text at specific position
    auto write_text = [](int x, int y, const char* text, uint8_t color) {
        volatile uint16_t* video = (volatile uint16_t*)0xB8000;
        int pos = y * 80 + x;
        for (int i = 0; text[i] != '\0' && (pos + i) < (80 * 25); i++) {
            if (x + i < 80) {  // Ensure we don't exceed line width
                video[pos + i] = (uint16_t)text[i] | ((uint16_t)color << 8);
            }
        }
    };
    
    write_text(0, 0, "SCos Boot", 0x0F);  // White on black
    
    // Small delay to ensure VGA is stable
    for (volatile int i = 0; i < 100000; i++);
    
    write_text(0, 1, "Serial init...", 0x0E);  // Yellow on black
    
    // Initialize serial first for debugging
    bool serial_ok = init_serial();
    if (!serial_ok) {
        write_text(0, 2, "Serial FAIL", 0x0C);  // Red on black
        // Continue without serial
    } else {
        write_text(0, 2, "Serial OK", 0x0A);  // Green on black
    }
    
    if (serial_ok) {
        serial_printf("SCos Kernel Starting...\n");
        serial_printf("Version: 0.1.0\n");
        show_memory_info();
        serial_printf("Global constructors called\n");
    }

    write_text(0, 3, "Constructors...", 0x0E);  // Yellow on black
    call_constructors();
    write_text(0, 4, "Constructors OK", 0x0B);  // Cyan on black

    write_text(0, 5, "Init subsystems...", 0x0E);  // Yellow on black
    
    if (serial_ok) {
        serial_printf("Starting subsystem initialization...\n");
    }
    
    // Try to initialize subsystems with simpler approach
    bool init_success = init_subsystems();
    
    if (!init_success) {
        write_text(0, 6, "Subsystem FAIL", 0x0C);  // Red on black
        if (serial_ok) {
            serial_printf("Subsystem initialization failed - continuing anyway\n");
        }
        // Don't panic, try to continue
    } else {
        write_text(0, 6, "Subsystems OK", 0x0A);  // Green on black
        if (serial_ok) {
            serial_printf("Subsystem initialization completed successfully\n");
        }
    }

    serial_printf("Creating desktop object...\n");
    Desktop desktop;
    
    serial_printf("Initializing desktop...\n");
    if (!desktop.init()) {
        write_text(0, 7, "CRITICAL: Desktop initialization failed!", 0x0C);  // Red on black
        serial_printf("Desktop initialization failed!\n");
        kernel_panic("Desktop environment initialization failed");
    }
    
    write_text(0, 7, "Desktop environment ready - SCos loaded!", 0x0E);  // Yellow on black
    serial_printf("Desktop initialized successfully\n");
    serial_printf("Desktop environment loaded successfully\n");
    serial_printf("Enabling interrupts and entering main loop...\n");

    asm volatile("sti");

    write_text(0, 8, "SCos boot complete - System ready!", 0x0F);  // Bright white on black
    serial_printf("Starting main kernel loop...\n");
    
    uint32_t tick_count = 0;
    uint32_t heartbeat_interval = 100000; // Slower heartbeat to reduce spam
    
    while (1) {
        // Handle desktop operations with error checking
        // Note: No exception handling in kernel space
        desktop.handle_events();
        desktop.update();

        tick_count++;
        if (tick_count % heartbeat_interval == 0) {
            serial_printf("Kernel heartbeat: %d\n", tick_count / heartbeat_interval);
            
            // Visual heartbeat indicator in bottom right corner
            static char heartbeat_chars[] = {'|', '-', '\\', '/'};
            static int heartbeat_index = 0;
            char heartbeat_text[2] = {heartbeat_chars[heartbeat_index], '\0'};
            write_text(79, 24, heartbeat_text, 0x0F);
            heartbeat_index = (heartbeat_index + 1) % 4;
        }

        // Yield CPU to prevent busy waiting
        asm volatile("pause");
        
        // Only halt if no interrupts are pending
        asm volatile("hlt");
    }
}

extern "C" void __cxa_pure_virtual() {
    kernel_panic("Pure virtual function call");
}

void* operator new(size_t size) {
    return kmalloc(size);
}

void* operator new[](size_t size) {
    return kmalloc(size);
}

void operator delete(void* ptr) {
    kfree(ptr);
}

void operator delete[](void* ptr) {
    kfree(ptr);
}

void operator delete(void* ptr, size_t) {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t) {
    kfree(ptr);
}