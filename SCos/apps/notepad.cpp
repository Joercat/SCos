
#include "notepad.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Local string function implementations for freestanding environment
static int calc_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void calc_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void calc_strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void calc_memset(void* ptr, int value, int size) {
    char* p = (char*)ptr;
    for (int i = 0; i < size; i++) {
        p[i] = value;
    }
}

// Notepad state
static char notepad_buffer[MAX_NOTEPAD_LINES * MAX_NOTEPAD_LINE_LENGTH];
static int cursor_x = 0;
static int cursor_y = 0;
static int notepad_window_id = -1;
static bool notepad_visible = false;
static int current_line = 0;
static int current_col = 0;

void Notepad::init() {
    calc_memset(notepad_buffer, 0, sizeof(notepad_buffer));
    cursor_x = 0;
    cursor_y = 0;
    notepad_window_id = -1;
    notepad_visible = false;
    current_line = 0;
    current_col = 0;
}

void Notepad::show() {
    if (notepad_visible) return;

    // Create window
    notepad_window_id = WindowManager::createWindow("Notepad", 15, 3, 50, 18);
    if (notepad_window_id >= 0) {
        notepad_visible = true;
        WindowManager::setActiveWindow(notepad_window_id);
        cursor_x = 2;
        cursor_y = 2;
        current_line = 0;
        current_col = 0;
        drawNotepad();
    }
}

void Notepad::hide() {
    if (!notepad_visible || notepad_window_id < 0) return;

    WindowManager::closeWindow(notepad_window_id);
    notepad_visible = false;
    notepad_window_id = -1;
}

bool Notepad::isVisible() {
    return notepad_visible;
}

void Notepad::handleInput(uint8_t key) {
    if (!notepad_visible) return;

    Window* win = WindowManager::getWindow(notepad_window_id);
    if (!win) return;

    switch (key) {
        case 0x01: // Escape
            hide();
            break;
        case 0x0E: // Backspace
            deleteChar();
            break;
        case 0x1C: // Enter
            newLine();
            break;
        default:
            // Handle printable characters
            if (key >= 0x02 && key <= 0x35) { // Various keys
                char c = 0;
                if (key >= 0x02 && key <= 0x0B) { // Number keys 1-9, 0
                    c = (key == 0x0B) ? '0' : ('1' + key - 0x02);
                } else if (key >= 0x10 && key <= 0x19) { // QWERTY row
                    const char qwerty[] = "qwertyuiop";
                    c = qwerty[key - 0x10];
                } else if (key >= 0x1E && key <= 0x26) { // ASDF row
                    const char asdf[] = "asdfghjkl";
                    c = asdf[key - 0x1E];
                } else if (key >= 0x2C && key <= 0x32) { // ZXCV row
                    const char zxcv[] = "zxcvbnm";
                    c = zxcv[key - 0x2C];
                } else if (key == 0x39) { // Space
                    c = ' ';
                }
                
                if (c != 0) {
                    insertChar(c);
                }
            }
            break;
    }
}

void Notepad::handleMouseClick(int x, int y) {
    if (!notepad_visible || notepad_window_id < 0) return;

    Window* win = WindowManager::getWindow(notepad_window_id);
    if (!win) return;

    // Check if click is within notepad content area
    if (x >= win->x + 1 && x < win->x + win->width - 1 &&
        y >= win->y + 1 && y < win->y + win->height - 1) {
        
        // Move cursor to clicked position
        int new_col = x - (win->x + 2);
        int new_line = y - (win->y + 2);
        
        if (new_line >= 0 && new_line < MAX_NOTEPAD_LINES) {
            current_line = new_line;
            if (new_col >= 0 && new_col < MAX_NOTEPAD_LINE_LENGTH) {
                current_col = new_col;
            }
            updateDisplay();
        }
    }
}

void Notepad::drawNotepad() {
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
    int display_lines = (win->height - 4 < MAX_NOTEPAD_LINES) ? win->height - 4 : MAX_NOTEPAD_LINES;
    int display_cols = (win->width - 4 < MAX_NOTEPAD_LINE_LENGTH) ? win->width - 4 : MAX_NOTEPAD_LINE_LENGTH;

    for (int line = 0; line < display_lines; line++) {
        for (int col = 0; col < display_cols; col++) {
            int buffer_idx = line * MAX_NOTEPAD_LINE_LENGTH + col;
            if (notepad_buffer[buffer_idx] != 0) {
                vga_put_char(win->x + 2 + col, win->y + 2 + line, 
                           notepad_buffer[buffer_idx], MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
            }
        }
    }

    // Draw cursor
    vga_put_char(win->x + 2 + current_col, win->y + 2 + current_line, 
                '_', MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
}

void Notepad::insertChar(char c) {
    if (current_line >= MAX_NOTEPAD_LINES || current_col >= MAX_NOTEPAD_LINE_LENGTH - 1) return;

    int buffer_idx = current_line * MAX_NOTEPAD_LINE_LENGTH + current_col;
    notepad_buffer[buffer_idx] = c;
    
    current_col++;
    if (current_col >= MAX_NOTEPAD_LINE_LENGTH - 1) {
        newLine();
    }
    
    updateDisplay();
}

void Notepad::deleteChar() {
    if (current_col > 0) {
        current_col--;
        int buffer_idx = current_line * MAX_NOTEPAD_LINE_LENGTH + current_col;
        notepad_buffer[buffer_idx] = 0;
    } else if (current_line > 0) {
        current_line--;
        current_col = MAX_NOTEPAD_LINE_LENGTH - 1;
        // Find the actual end of the line
        while (current_col > 0) {
            int buffer_idx = current_line * MAX_NOTEPAD_LINE_LENGTH + current_col - 1;
            if (notepad_buffer[buffer_idx] != 0) break;
            current_col--;
        }
    }
    updateDisplay();
}

void Notepad::newLine() {
    if (current_line < MAX_NOTEPAD_LINES - 1) {
        current_line++;
        current_col = 0;
        updateDisplay();
    }
}

void Notepad::moveCursor(int dx, int dy) {
    int new_col = current_col + dx;
    int new_line = current_line + dy;
    
    if (new_col >= 0 && new_col < MAX_NOTEPAD_LINE_LENGTH) {
        current_col = new_col;
    }
    
    if (new_line >= 0 && new_line < MAX_NOTEPAD_LINES) {
        current_line = new_line;
    }
    
    updateDisplay();
}

void Notepad::updateDisplay() {
    drawNotepad();
}

// VGA function implementations
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

// Legacy function support for compatibility
void openNotepad(const char* initial_content) {
    Notepad::init();
    if (initial_content && calc_strlen(initial_content) > 0) {
        // Copy initial content to buffer
        for (int i = 0; i < calc_strlen(initial_content) && i < sizeof(notepad_buffer) - 1; i++) {
            notepad_buffer[i] = initial_content[i];
        }
    }
    Notepad::show();
}

void closeNotepad() {
    Notepad::hide();
}

bool isNotepadVisible() {
    return Notepad::isVisible();
}

void drawNotepadContent() {
    Notepad::updateDisplay();
}

void handleNotepadInput(uint8_t key) {
    Notepad::handleInput(key);
}
