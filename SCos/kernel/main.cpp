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
    serial_printf("Kernel loaded at: 0x1000\n");
    serial_printf("Kernel end: 0x%x\n", _kernel_end);
    serial_printf("Available memory starts at: 0x%x\n", _kernel_end);
}

extern "C" void _start() {
    // Clear screen and show startup message
    volatile char* video = (volatile char*)0xB8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video[i] = ' ';
        video[i + 1] = 0x07; // White on black
    }
    
    // Display startup message on screen
    const char* msg = "SCos Kernel Starting... Version 0.1.0";
    for (int i = 0; msg[i] != '\0'; i++) {
        video[i * 2] = msg[i];
        video[i * 2 + 1] = 0x0F; // Bright white on black
    }
    
    // Show early boot message immediately
    const char* boot_msg = "Kernel loaded successfully!";
    for (int i = 0; boot_msg[i] != '\0'; i++) {
        video[160 + i * 2] = boot_msg[i]; // Second line
        video[160 + i * 2 + 1] = 0x0A; // Green on black
    }
    
    // Initialize serial first for debugging
    if (!init_serial()) {
        const char* serial_error = "CRITICAL: Serial initialization failed!";
        for (int i = 0; serial_error[i] != '\0'; i++) {
            video[320 + i * 2] = serial_error[i]; // Third line
            video[320 + i * 2 + 1] = 0x0C; // Red on black
        }
        // Continue without serial
    }
    
    serial_printf("SCos Kernel Starting...\n");
    serial_printf("Version: 0.1.0\n");

    show_memory_info();

    // Show memory info on screen
    const char* mem_msg = "Memory initialized";
    for (int i = 0; mem_msg[i] != '\0'; i++) {
        video[480 + i * 2] = mem_msg[i]; // Fourth line
        video[480 + i * 2 + 1] = 0x0E; // Yellow on black
    }

    call_constructors();
    serial_printf("Global constructors called\n");
    
    // Show constructors completed
    const char* ctor_msg = "Global constructors completed";
    for (int i = 0; ctor_msg[i] != '\0'; i++) {
        video[640 + i * 2] = ctor_msg[i]; // Fifth line
        video[640 + i * 2 + 1] = 0x0B; // Cyan on black
    }

    if (!init_subsystems()) {
        // Show error on screen
        const char* error_msg = "CRITICAL: Subsystem initialization failed!";
        for (int i = 0; error_msg[i] != '\0'; i++) {
            video[160 + i * 2] = error_msg[i]; // Second line
            video[160 + i * 2 + 1] = 0x0C; // Red on black
        }
        kernel_panic("Critical subsystem initialization failed");
    }

    // Show subsystems OK message
    const char* ok_msg = "All subsystems initialized successfully";
    for (int i = 0; ok_msg[i] != '\0'; i++) {
        video[160 + i * 2] = ok_msg[i]; // Second line
        video[160 + i * 2 + 1] = 0x0A; // Green on black
    }

    Desktop desktop;
    if (!desktop.init()) {
        const char* desktop_error = "CRITICAL: Desktop initialization failed!";
        for (int i = 0; desktop_error[i] != '\0'; i++) {
            video[320 + i * 2] = desktop_error[i]; // Third line
            video[320 + i * 2 + 1] = 0x0C; // Red on black
        }
        kernel_panic("Desktop environment initialization failed");
    }

    // Show desktop ready message
    const char* desktop_msg = "Desktop environment ready - SCos loaded!";
    for (int i = 0; desktop_msg[i] != '\0'; i++) {
        video[320 + i * 2] = desktop_msg[i]; // Third line
        video[320 + i * 2 + 1] = 0x0E; // Yellow on black
    }

    serial_printf("Desktop environment loaded successfully\n");
    serial_printf("Enabling interrupts and entering main loop...\n");

    asm volatile("sti");

    // Show final success message
    const char* success_msg = "SCos boot complete - System ready!";
    for (int i = 0; success_msg[i] != '\0'; i++) {
        video[800 + i * 2] = success_msg[i]; // Sixth line
        video[800 + i * 2 + 1] = 0x0F; // Bright white on black
    }

    uint32_t tick_count = 0;
    uint32_t heartbeat_interval = 100000; // Faster heartbeat for debugging
    
    while (1) {
        // Handle desktop operations with simple error checking
        desktop.handle_events();
        desktop.update();

        tick_count++;
        if (tick_count % heartbeat_interval == 0) {
            serial_printf("Kernel heartbeat: %d\n", tick_count / heartbeat_interval);
            
            // Visual heartbeat indicator
            static char heartbeat_char = '|';
            video[1920] = heartbeat_char; // Bottom right corner
            video[1921] = 0x0F;
            heartbeat_char = (heartbeat_char == '|') ? '-' : '|';
        }

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