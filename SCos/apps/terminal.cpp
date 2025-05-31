
#include "terminal.hpp"
#include "../drivers/keyboard.hpp"
#include "../memory/heap.hpp"
#include "../debug/serial.hpp"
#include "../include/stddef.h"
#include "../include/string.h"

// Add strncmp function since it's not available in our custom environment
extern "C" {
int strncmp(const char* str1, const char* str2, size_t n) {
    while (n && *str1 && (*str1 == *str2)) {
        ++str1;
        ++str2;
        --n;
    }
    if (n == 0) {
        return 0;
    } else {
        return (*(unsigned char*)str1 - *(unsigned char*)str2);
    }
}
}

Terminal::Terminal() {
    screen = (volatile char*)0xB8000;
    cursor_x = 0;
    cursor_y = 0;
    current_attr = 0x0F;
    command_pos = 0;
    history_pos = -1;
    
    for (int i = 0; i < HISTORY_SIZE; i++) {
        history[i] = nullptr;
    }
    
    clear();
    print_banner();
    show_prompt();
}

Terminal::~Terminal() {
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (history[i]) {
            kfree(history[i]);
        }
    }
}

void Terminal::clear() {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT * 2; i += 2) {
        screen[i] = ' ';
        screen[i + 1] = current_attr;
    }
    cursor_x = 0;
    cursor_y = 0;
}

void Terminal::put_char(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            int pos = (cursor_y * SCREEN_WIDTH + cursor_x) * 2;
            screen[pos] = ' ';
            screen[pos + 1] = current_attr;
        }
        return;
    }
    
    if (cursor_x >= SCREEN_WIDTH) {
        newline();
    }
    
    int pos = (cursor_y * SCREEN_WIDTH + cursor_x) * 2;
    screen[pos] = c;
    screen[pos + 1] = current_attr;
    cursor_x++;
}

void Terminal::print(const char* str) {
    while (*str) {
        put_char(*str++);
    }
}

void Terminal::newline() {
    cursor_x = 0;
    cursor_y++;
    
    if (cursor_y >= SCREEN_HEIGHT) {
        scroll_up();
        cursor_y = SCREEN_HEIGHT - 1;
    }
}

void Terminal::scroll_up() {
    for (int y = 0; y < SCREEN_HEIGHT - 1; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int src = ((y + 1) * SCREEN_WIDTH + x) * 2;
            int dst = (y * SCREEN_WIDTH + x) * 2;
            screen[dst] = screen[src];
            screen[dst + 1] = screen[src + 1];
        }
    }
    
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int pos = ((SCREEN_HEIGHT - 1) * SCREEN_WIDTH + x) * 2;
        screen[pos] = ' ';
        screen[pos + 1] = current_attr;
    }
}

void Terminal::print_banner() {
    current_attr = 0x0B;
    print("SCos Terminal v1.0\n");
    current_attr = 0x07;
    print("Type 'help' for available commands.\n\n");
    current_attr = 0x0F;
}

void Terminal::show_prompt() {
    current_attr = 0x0A;
    print("SCos> ");
    current_attr = 0x0F;
    prompt_x = cursor_x;
}

char Terminal::scancode_to_char(uint8_t scancode, bool shift) {
    static const char normal_map[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
        0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
        '*', 0, ' '
    };
    
    static const char shift_map[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
        0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
        '*', 0, ' '
    };
    
    if (scancode >= sizeof(normal_map)) return 0;
    return shift ? shift_map[scancode] : normal_map[scancode];
}

void Terminal::add_to_history(const char* cmd) {
    for (int i = HISTORY_SIZE - 1; i > 0; i--) {
        history[i] = history[i - 1];
    }
    
    size_t len = strlen(cmd);
    history[0] = (char*)kmalloc(len + 1);
    if (history[0]) {
        strcpy(history[0], cmd);
    }
}

void Terminal::execute_command(const char* cmd) {
    if (strlen(cmd) == 0) return;
    
    add_to_history(cmd);
    newline();
    
    if (strcmp(cmd, "help") == 0) {
        print("Available commands:\n");
        print("  help     - Show this help\n");
        print("  clear    - Clear screen\n");
        print("  version  - Show OS version\n");
        print("  memory   - Show memory info\n");
        print("  reboot   - Restart system\n");
        print("  echo     - Echo text\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        clear();
        print_banner();
    }
    else if (strcmp(cmd, "version") == 0) {
        print("SCos version 0.1.0\n");
        print("Built with love and assembly\n");
    }
    else if (strcmp(cmd, "memory") == 0) {
        print("Memory information:\n");
        heap_stats();
    }
    else if (strcmp(cmd, "reboot") == 0) {
        print("Rebooting system...\n");
        asm volatile("int $0x19");
    }
    else if (strncmp(cmd, "echo ", 5) == 0) {
        print(cmd + 5);
        newline();
    }
    else {
        current_attr = 0x0C;
        print("Unknown command: ");
        print(cmd);
        newline();
        current_attr = 0x0F;
    }
    
    show_prompt();
}

void Terminal::handle_input() {
    uint8_t scancode = readScancode();
    if (scancode == 0) return;
    
    static bool shift_pressed = false;
    static bool ctrl_pressed = false;
    
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = false;
        return;
    }
    if (scancode == 0x1D) {
        ctrl_pressed = true;
        return;
    }
    if (scancode == 0x9D) {
        ctrl_pressed = false;
        return;
    }
    
    if (scancode & 0x80) return;
    
    if (scancode == 0x0E) {
        if (command_pos > 0 && cursor_x > prompt_x) {
            command_pos--;
            put_char('\b');
        }
        return;
    }
    
    if (scancode == 0x1C) {
        command_buffer[command_pos] = '\0';
        execute_command(command_buffer);
        command_pos = 0;
        return;
    }
    
    if (scancode == 0x48) {
        if (history_pos < HISTORY_SIZE - 1 && history[history_pos + 1]) {
            history_pos++;
            while (cursor_x > prompt_x) {
                put_char('\b');
                command_pos--;
            }
            strcpy(command_buffer, history[history_pos]);
            command_pos = strlen(command_buffer);
            print(command_buffer);
        }
        return;
    }
    
    if (scancode == 0x50) {
        if (history_pos > -1) {
            while (cursor_x > prompt_x) {
                put_char('\b');
                command_pos--;
            }
            history_pos--;
            if (history_pos >= 0) {
                strcpy(command_buffer, history[history_pos]);
                command_pos = strlen(command_buffer);
                print(command_buffer);
            } else {
                command_pos = 0;
            }
        }
        return;
    }
    
    char c = scancode_to_char(scancode, shift_pressed);
    if (c && command_pos < MAX_COMMAND_LENGTH - 1) {
        command_buffer[command_pos++] = c;
        put_char(c);
    }
}

void Terminal::run() {
    while (true) {
        handle_input();
    }
}

void Terminal::handleInput(uint8_t key) {
    // Static method for external input handling
    // For now, just ignore - proper implementation would need global terminal instance
    (void)key; // Suppress unused parameter warning
}
