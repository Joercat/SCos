#include "terminal.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Forward declarations
void drawTerminalContent();
void executeCommand();
void handleTerminalInput(uint8_t key);

// Local string function implementations for freestanding environment
static int terminal_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void terminal_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void terminal_strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// Terminal state
static char terminal_buffer[2048];
static char current_line[256];
static int terminal_window_id = -1;
static bool terminal_visible = false;
static int cursor_pos = 0;

void runTerminal() {
    if (terminal_visible) return;

    // Initialize terminal
    terminal_strcpy(terminal_buffer, "SCos Terminal v1.0\n> ");
    terminal_strcpy(current_line, "");
    cursor_pos = 0;

    // Create terminal window
    terminal_window_id = WindowManager::createWindow("Terminal", 10, 5, 60, 15);
    if (terminal_window_id >= 0) {
        terminal_visible = true;
        WindowManager::setActiveWindow(terminal_window_id);
        drawTerminalContent();
    }
}

void closeTerminal() {
    if (!terminal_visible || terminal_window_id < 0) return;

    WindowManager::closeWindow(terminal_window_id);
    terminal_visible = false;
    terminal_window_id = -1;
}

bool isTerminalVisible() {
    return terminal_visible;
}

void drawTerminalContent() {
    if (!terminal_visible || terminal_window_id < 0) return;

    Window* win = WindowManager::getWindow(terminal_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int content_start_y = win->y + 1;
    int start_x = win->x + 1;

    // Clear content area
    for (int y = content_start_y; y < win->y + win->height - 1; ++y) {
        for (int x = start_x; x < win->x + win->width - 1; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x0F; // White on black
        }
    }

    // Draw terminal buffer
    int line = 0;
    int col = 0;
    for (int i = 0; terminal_buffer[i] && line < win->height - 3; ++i) {
        if (terminal_buffer[i] == '\n') {
            line++;
            col = 0;
        } else if (col < win->width - 3) {
            int idx = 2 * ((content_start_y + line) * 80 + (start_x + col));
            video[idx] = terminal_buffer[i];
            video[idx + 1] = 0x0F; // White on black
            col++;
        }
    }

    // Draw current input line
    for (int i = 0; current_line[i] && col < win->width - 3; ++i) {
        int idx = 2 * ((content_start_y + line) * 80 + (start_x + col));
        video[idx] = current_line[i];
        video[idx + 1] = 0x0F;
        col++;
    }

    // Draw cursor
    if (col < win->width - 3) {
        int idx = 2 * ((content_start_y + line) * 80 + (start_x + col));
        video[idx] = '_';
        video[idx + 1] = 0x0F;
    }
}

void handleTerminalInput(uint8_t key) {
    if (!terminal_visible) return;

    switch (key) {
        case 0x01: // Escape
            closeTerminal();
            break;
        case 0x0E: // Backspace
            if (cursor_pos > 0) {
                current_line[--cursor_pos] = '\0';
                drawTerminalContent();
            }
            break;
        case 0x1C: // Enter
            executeCommand();
            break;
        default:
            // Handle printable characters (simplified)
            if (key >= 0x02 && key <= 0x0D && cursor_pos < 255) { // Number keys
                char c = '0' + (key - 1);
                if (key == 0x0B) c = '0';
                current_line[cursor_pos++] = c;
                current_line[cursor_pos] = '\0';
                drawTerminalContent();
            }
            break;
    }
}

void executeCommand() {
    // Add command to buffer
    terminal_strcat(terminal_buffer, current_line);
    terminal_strcat(terminal_buffer, "\n");

    // Simple command processing
    if (current_line[0] == 'h' && current_line[1] == 'e') { // help
        terminal_strcat(terminal_buffer, "Available commands:\n");
        terminal_strcat(terminal_buffer, "  help - Show this help\n");
        terminal_strcat(terminal_buffer, "  clear - Clear screen\n");
        terminal_strcat(terminal_buffer, "  exit - Close terminal\n");
    } else if (current_line[0] == 'c' && current_line[1] == 'l') { // clear
        terminal_strcpy(terminal_buffer, "SCos Terminal v1.0\n");
    } else if (current_line[0] == 'e' && current_line[1] == 'x') { // exit
        closeTerminal();
        return;
    } else if (current_line[0] != '\0') {
        terminal_strcat(terminal_buffer, "Command not found: ");
        terminal_strcat(terminal_buffer, current_line);
        terminal_strcat(terminal_buffer, "\n");
    }

    // Add new prompt
    terminal_strcat(terminal_buffer, "> ");

    // Clear current line
    terminal_strcpy(current_line, "");
    cursor_pos = 0;

    drawTerminalContent();
}