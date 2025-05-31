#include "notepad.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Remove duplicate color definitions - they're already in window_manager.hpp

// Local string function implementations for freestanding environment
static int strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void memset(void* ptr, int value, int size) {
    char* p = (char*)ptr;
    for (int i = 0; i < size; i++) {
        p[i] = value;
    }
}

// Notepad state
static char notepad_buffer[4096];
static int cursor_x = 0;
static int cursor_y = 0;
static int notepad_window_id = -1;
static bool notepad_visible = false;

void openNotepad(const char* initial_content) {
    if (notepad_visible) return;

    // Initialize buffer
    memset(notepad_buffer, 0, sizeof(notepad_buffer));
    if (initial_content) {
        strcpy(notepad_buffer, initial_content);
    }

    // Create window
    notepad_window_id = WindowManager::createWindow("Notepad", 15, 3, 50, 18);
    if (notepad_window_id >= 0) {
        notepad_visible = true;
        WindowManager::setActiveWindow(notepad_window_id);
        cursor_x = 2;
        cursor_y = 2;
        drawNotepadContent();
    }
}

void closeNotepad() {
    if (!notepad_visible || notepad_window_id < 0) return;

    WindowManager::closeWindow(notepad_window_id);
    notepad_visible = false;
    notepad_window_id = -1;
}

bool isNotepadVisible() {
    return notepad_visible;
}

void drawNotepadContent() {
    if (!notepad_visible || notepad_window_id < 0) return;

    Window* win = WindowManager::getWindow(notepad_window_id);
    if (!win) return;

    // Clear content area
    for (int y = win->y + 1; y < win->y + win->height - 1; y++) {
        for (int x = win->x + 1; x < win->x + win->width - 1; x++) {
            vga_put_char(x, y, ' ', MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
        }
    }

    // Draw text content
    int line = 0;
    int col = 0;
    int content_start_x = win->x + 2;
    int content_start_y = win->y + 2;

    for (int i = 0; notepad_buffer[i] && line < win->height - 4; i++) {
        if (notepad_buffer[i] == '\n') {
            line++;
            col = 0;
        } else if (col < win->width - 4) {
            vga_put_char(content_start_x + col, content_start_y + line, 
                        notepad_buffer[i], MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
            col++;
        }
    }

    // Draw cursor
    vga_put_char(content_start_x + cursor_x - 2, content_start_y + cursor_y - 2, 
                '_', MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
}

void handleNotepadInput(uint8_t key) {
    if (!notepad_visible) return;

    Window* win = WindowManager::getWindow(notepad_window_id);
    if (!win) return;

    switch (key) {
        case 0x01: // Escape
            closeNotepad();
            break;
        case 0x0E: // Backspace
            if (strlen(notepad_buffer) > 0) {
                notepad_buffer[strlen(notepad_buffer) - 1] = '\0';
                if (cursor_x > 2) cursor_x--;
                drawNotepadContent();
            }
            break;
        case 0x1C: // Enter
            if (strlen(notepad_buffer) < sizeof(notepad_buffer) - 1) {
                strcat(notepad_buffer, "\n");
                cursor_y++;
                cursor_x = 2;
                drawNotepadContent();
            }
            break;
        default:
            // Handle printable characters
            if (key >= 0x02 && key <= 0x0D) { // Number keys
                char c = '0' + (key - 1);
                if (key == 0x0B) c = '0';
                if (strlen(notepad_buffer) < sizeof(notepad_buffer) - 1) {
                    char temp[2] = {c, '\0'};
                    strcat(notepad_buffer, temp);
                    cursor_x++;
                    drawNotepadContent();
                }
            }
            break;
    }
}

// VGA function implementations - remove static keyword
void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 80 || y >= 25 || x < 0 || y < 0) return;

    volatile char* video = (volatile char*)0xB8000;
    int idx = 2 * (y * 80 + x);
    video[idx] = c;
    video[idx + 1] = color;
}

void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i]; ++i) {
        vga_put_char(x + i, y, str[i], color);
    }
}

void vga_clear_screen(uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video[i] = ' ';
        video[i + 1] = color;
    }
}

void vga_clear_line(int y, uint8_t color) {
    if (y >= 25 || y < 0) return;

    volatile char* video = (volatile char*)0xB8000;
    for (int x = 0; x < 80; ++x) {
        int idx = 2 * (y * 80 + x);
        video[idx] = ' ';
        video[idx + 1] = color;
    }
}