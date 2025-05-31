#include <stdint.h>

// VGA text mode constants
#define VGA_BUFFER ((volatile char*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BYTES_PER_CHAR 2

// Color attributes
#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x01
#define COLOR_GREEN 0x02
#define COLOR_CYAN 0x03
#define COLOR_RED 0x04
#define COLOR_MAGENTA 0x05
#define COLOR_BROWN 0x06
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_DARK_GRAY 0x08
#define COLOR_LIGHT_BLUE 0x09
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_LIGHT_CYAN 0x0B
#define COLOR_LIGHT_RED 0x0C
#define COLOR_LIGHT_MAGENTA 0x0D
#define COLOR_YELLOW 0x0E
#define COLOR_WHITE 0x0F

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

// Notepad configuration
#define MAX_TEXT_SIZE 4096
#define CONTENT_START_Y 3
#define CONTENT_HEIGHT 20
#define STATUS_BAR_Y 23

// Notepad state
typedef struct {
    char text[MAX_TEXT_SIZE];
    int cursor_x;
    int cursor_y;
    int scroll_offset;
    int text_length;
    int modified;
    char filename[32];
    int insert_mode;  // 1 for insert, 0 for overwrite
    int selection_start;
    int selection_end;
    int word_wrap;
} NotepadState;

static NotepadState notepad = {
    .text = {0},
    .cursor_x = 0,
    .cursor_y = 0,
    .scroll_offset = 0,
    .text_length = 0,
    .modified = 0,
    .filename = "Untitled.txt",
    .insert_mode = 1,
    .selection_start = -1,
    .selection_end = -1,
    .word_wrap = 1
};

// Menu system
typedef struct {
    const char* text;
    char shortcut;
    int enabled;
} MenuItem;

static MenuItem file_menu[] = {
    {"New", 'N', 1},
    {"Open", 'O', 1},
    {"Save", 'S', 1},
    {"Save As", 'A', 1},
    {"---", 0, 0},
    {"Exit", 'X', 1}
};

static MenuItem edit_menu[] = {
    {"Undo", 'Z', 0},
    {"---", 0, 0},
    {"Cut", 'X', 1},
    {"Copy", 'C', 1},
    {"Paste", 'V', 1},
    {"---", 0, 0},
    {"Select All", 'A', 1},
    {"Find", 'F', 1}
};

static MenuItem view_menu[] = {
    {"Word Wrap", 'W', 1},
    {"Status Bar", 'B', 1},
    {"---", 0, 0},
    {"Zoom In", '+', 1},
    {"Zoom Out", '-', 1}
};

// Utility functions
static int strlen_custom(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void strcpy_custom(char* dest, const char* src) {
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile char* pos = VGA_BUFFER + (y * VGA_WIDTH + x) * VGA_BYTES_PER_CHAR;
        pos[0] = c;
        pos[1] = color;
    }
}

static void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < VGA_WIDTH; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

static void vga_clear_screen(uint8_t color) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_put_char(x, y, ' ', color);
        }
    }
}

static void vga_clear_line(int y, uint8_t color) {
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put_char(x, y, ' ', color);
    }
}

static void int_to_string(int value, char* buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    int i = 0;
    int temp = value;
    
    while (temp > 0) {
        buffer[i++] = '0' + (temp % 10);
        temp /= 10;
    }
    buffer[i] = '\0';
    
    // Reverse the string
    for (int j = 0; j < i / 2; j++) {
        char tmp = buffer[j];
        buffer[j] = buffer[i - 1 - j];
        buffer[i - 1 - j] = tmp;
    }
}

// Text manipulation functions
static void text_insert_char(char c) {
    if (notepad.text_length >= MAX_TEXT_SIZE - 1) return;
    
    int cursor_pos = notepad.cursor_y * VGA_WIDTH + notepad.cursor_x;
    
    // Shift text right
    for (int i = notepad.text_length; i > cursor_pos; i--) {
        notepad.text[i] = notepad.text[i - 1];
    }
    
    notepad.text[cursor_pos] = c;
    notepad.text_length++;
    notepad.modified = 1;
    
    // Move cursor
    if (c == '\n') {
        notepad.cursor_x = 0;
        notepad.cursor_y++;
    } else {
        notepad.cursor_x++;
        if (notepad.cursor_x >= VGA_WIDTH) {
            notepad.cursor_x = 0;
            notepad.cursor_y++;
        }
    }
}

static void text_delete_char() {
    if (notepad.text_length == 0) return;
    
    int cursor_pos = notepad.cursor_y * VGA_WIDTH + notepad.cursor_x;
    if (cursor_pos == 0) return;
    
    // Move cursor back
    if (notepad.cursor_x > 0) {
        notepad.cursor_x--;
    } else if (notepad.cursor_y > 0) {
        notepad.cursor_y--;
        notepad.cursor_x = VGA_WIDTH - 1;
        // Find actual end of previous line
        while (notepad.cursor_x > 0 && notepad.text[cursor_pos - 1] != '\n') {
            notepad.cursor_x--;
            cursor_pos--;
        }
    }
    
    cursor_pos = notepad.cursor_y * VGA_WIDTH + notepad.cursor_x;
    
    // Shift text left
    for (int i = cursor_pos; i < notepad.text_length - 1; i++) {
        notepad.text[i] = notepad.text[i + 1];
    }
    
    notepad.text_length--;
    notepad.modified = 1;
}

static int count_lines() {
    int lines = 1;
    for (int i = 0; i < notepad.text_length; i++) {
        if (notepad.text[i] == '\n') lines++;
    }
    return lines;
}

static int count_words() {
    int words = 0;
    int in_word = 0;
    
    for (int i = 0; i < notepad.text_length; i++) {
        char c = notepad.text[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    return words;
}

// Drawing functions
static void draw_menu_bar() {
    uint8_t menu_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY);
    uint8_t shortcut_color = MAKE_COLOR(COLOR_RED, COLOR_LIGHT_GRAY);
    
    vga_clear_line(0, menu_color);
    
    vga_put_string(2, 0, "File", menu_color);
    vga_put_string(8, 0, "Edit", menu_color);
    vga_put_string(14, 0, "View", menu_color);
    vga_put_string(20, 0, "Help", menu_color);
    
    // File modified indicator
    if (notepad.modified) {
        vga_put_char(70, 0, '*', MAKE_COLOR(COLOR_RED, COLOR_LIGHT_GRAY));
    }
}

static void draw_title_bar() {
    uint8_t title_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    vga_clear_line(1, title_color);
    
    char title[60];
    int pos = 0;
    
    // Build title string
    for (int i = 0; notepad.filename[i] && pos < 30; i++) {
        title[pos++] = notepad.filename[i];
    }
    
    if (notepad.modified) {
        title[pos++] = '*';
    }
    
    title[pos++] = ' ';
    title[pos++] = '-';
    title[pos++] = ' ';
    
    const char* app_name = "SCos Notepad";
    for (int i = 0; app_name[i] && pos < 59; i++) {
        title[pos++] = app_name[i];
    }
    title[pos] = '\0';
    
    vga_put_string(2, 1, title, title_color);
    
    // Window controls
    vga_put_string(74, 1, "- [] X", title_color);
}

static void draw_toolbar() {
    uint8_t toolbar_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY);
    vga_clear_line(2, toolbar_color);
    
    // Toolbar buttons
    vga_put_string(2, 2, "[New]", toolbar_color);
    vga_put_string(8, 2, "[Open]", toolbar_color);
    vga_put_string(15, 2, "[Save]", toolbar_color);
    vga_put_string(22, 2, "|", MAKE_COLOR(COLOR_DARK_GRAY, COLOR_LIGHT_GRAY));
    vga_put_string(24, 2, "[Cut]", toolbar_color);
    vga_put_string(30, 2, "[Copy]", toolbar_color);
    vga_put_string(37, 2, "[Paste]", toolbar_color);
    vga_put_string(45, 2, "|", MAKE_COLOR(COLOR_DARK_GRAY, COLOR_LIGHT_GRAY));
    vga_put_string(47, 2, "[Find]", toolbar_color);
    
    // Insert/Overwrite mode
    const char* mode = notepad.insert_mode ? "INS" : "OVR";
    vga_put_string(70, 2, mode, MAKE_COLOR(COLOR_BLUE, COLOR_LIGHT_GRAY));
}

static void draw_content_area() {
    uint8_t text_color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);
    uint8_t selected_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    
    // Clear content area
    for (int y = CONTENT_START_Y; y < CONTENT_START_Y + CONTENT_HEIGHT; y++) {
        vga_clear_line(y, text_color);
    }
    
    // Draw text content
    int text_pos = notepad.scroll_offset * VGA_WIDTH;
    int screen_y = CONTENT_START_Y;
    int screen_x = 0;
    
    for (int i = text_pos; i < notepad.text_length && screen_y < CONTENT_START_Y + CONTENT_HEIGHT; i++) {
        char c = notepad.text[i];
        
        if (c == '\n') {
            screen_y++;
            screen_x = 0;
        } else if (c == '\t') {
            // Tab = 4 spaces
            for (int t = 0; t < 4 && screen_x < VGA_WIDTH; t++) {
                vga_put_char(screen_x, screen_y, ' ', text_color);
                screen_x++;
            }
        } else if (c >= 32 && c <= 126) { // Printable characters
            uint8_t char_color = text_color;
            
            // Check if character is selected
            if (notepad.selection_start != -1 && notepad.selection_end != -1) {
                int start = (notepad.selection_start < notepad.selection_end) ? notepad.selection_start : notepad.selection_end;
                int end = (notepad.selection_start > notepad.selection_end) ? notepad.selection_start : notepad.selection_end;
                if (i >= start && i <= end) {
                    char_color = selected_color;
                }
            }
            
            vga_put_char(screen_x, screen_y, c, char_color);
            screen_x++;
            
            if (screen_x >= VGA_WIDTH) {
                screen_y++;
                screen_x = 0;
            }
        }
    }
    
    // Draw cursor
    if (notepad.cursor_y >= notepad.scroll_offset && 
        notepad.cursor_y < notepad.scroll_offset + CONTENT_HEIGHT) {
        int cursor_screen_y = CONTENT_START_Y + (notepad.cursor_y - notepad.scroll_offset);
        vga_put_char(notepad.cursor_x, cursor_screen_y, '_', MAKE_COLOR(COLOR_BLACK, COLOR_YELLOW));
    }
}

static void draw_status_bar() {
    uint8_t status_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY);
    vga_clear_line(STATUS_BAR_Y, status_color);
    
    // Line and column info
    char line_info[20];
    char col_str[8], line_str[8];
    int_to_string(notepad.cursor_y + 1, line_str);
    int_to_string(notepad.cursor_x + 1, col_str);
    
    int pos = 0;
    const char* ln_text = "Ln ";
    for (int i = 0; ln_text[i]; i++) line_info[pos++] = ln_text[i];
    for (int i = 0; line_str[i]; i++) line_info[pos++] = line_str[i];
    line_info[pos++] = ',';
    line_info[pos++] = ' ';
    const char* col_text = "Col ";
    for (int i = 0; col_text[i]; i++) line_info[pos++] = col_text[i];
    for (int i = 0; col_str[i]; i++) line_info[pos++] = col_str[i];
    line_info[pos] = '\0';
    
    vga_put_string(2, STATUS_BAR_Y, line_info, status_color);
    
    // Character count
    char char_info[20];
    char char_str[8];
    int_to_string(notepad.text_length, char_str);
    pos = 0;
    for (int i = 0; char_str[i]; i++) char_info[pos++] = char_str[i];
    const char* chars_text = " chars";
    for (int i = 0; chars_text[i]; i++) char_info[pos++] = chars_text[i];
    char_info[pos] = '\0';
    
    vga_put_string(20, STATUS_BAR_Y, char_info, status_color);
    
    // Word count
    char word_info[15];
    char word_str[8];
    int_to_string(count_words(), word_str);
    pos = 0;
    for (int i = 0; word_str[i]; i++) word_info[pos++] = word_str[i];
    const char* words_text = " words";
    for (int i = 0; words_text[i]; i++) word_info[pos++] = words_text[i];
    word_info[pos] = '\0';
    
    vga_put_string(35, STATUS_BAR_Y, word_info, status_color);
    
    // Encoding and mode info
    vga_put_string(55, STATUS_BAR_Y, "UTF-8", status_color);
    vga_put_string(65, STATUS_BAR_Y, notepad.word_wrap ? "Wrap" : "NoWrap", status_color);
}

// Main notepad function
void openNotepad(const char* content) {
    // Initialize with content if provided
    if (content) {
        strcpy_custom(notepad.text, content);
        notepad.text_length = strlen_custom(content);
        notepad.modified = 0;
    }
    
    // Clear screen
    vga_clear_screen(MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    
    // Draw interface
    draw_menu_bar();
    draw_title_bar();
    draw_toolbar();
    draw_content_area();
    draw_status_bar();
}

// Navigation and editing functions (called by keyboard handler)
void notepad_cursor_left() {
    if (notepad.cursor_x > 0) {
        notepad.cursor_x--;
    } else if (notepad.cursor_y > 0) {
        notepad.cursor_y--;
        notepad.cursor_x = VGA_WIDTH - 1;
    }
    draw_content_area();
    draw_status_bar();
}

void notepad_cursor_right() {
    int cursor_pos = notepad.cursor_y * VGA_WIDTH + notepad.cursor_x;
    if (cursor_pos < notepad.text_length) {
        notepad.cursor_x++;
        if (notepad.cursor_x >= VGA_WIDTH) {
            notepad.cursor_x = 0;
            notepad.cursor_y++;
        }
    }
    draw_content_area();
    draw_status_bar();
}

void notepad_cursor_up() {
    if (notepad.cursor_y > 0) {
        notepad.cursor_y--;
        if (notepad.cursor_y < notepad.scroll_offset) {
            notepad.scroll_offset = notepad.cursor_y;
            draw_content_area();
        }
    }
    draw_content_area();
    draw_status_bar();
}

void notepad_cursor_down() {
    int max_lines = count_lines();
    if (notepad.cursor_y < max_lines - 1) {
        notepad.cursor_y++;
        if (notepad.cursor_y >= notepad.scroll_offset + CONTENT_HEIGHT) {
            notepad.scroll_offset++;
            draw_content_area();
        }
    }
    draw_content_area();
    draw_status_bar();
}

void notepad_insert_char(char c) {
    text_insert_char(c);
    draw_content_area();
    draw_status_bar();
    draw_title_bar(); // Update modified indicator
}

void notepad_backspace() {
    text_delete_char();
    draw_content_area();
    draw_status_bar();
    draw_title_bar(); // Update modified indicator
}

void notepad_enter() {
    text_insert_char('\n');
    draw_content_area();
    draw_status_bar();
}

void notepad_tab() {
    text_insert_char('\t');
    draw_content_area();
    draw_status_bar();
}

// File operations
void notepad_new() {
    notepad.text[0] = '\0';
    notepad.text_length = 0;
    notepad.cursor_x = 0;
    notepad.cursor_y = 0;
    notepad.scroll_offset = 0;
    notepad.modified = 0;
    strcpy_custom(notepad.filename, "Untitled.txt");
    openNotepad(0);
}

void notepad_save() {
    // In a real OS, this would write to filesystem
    notepad.modified = 0;
    draw_title_bar();
    
    // Show save confirmation
    uint8_t msg_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GREEN);
    vga_put_string(50, STATUS_BAR_Y, "File saved!", msg_color);
}

// Advanced features
void notepad_toggle_word_wrap() {
    notepad.word_wrap = !notepad.word_wrap;
    draw_content_area();
    draw_status_bar();
}

void notepad_select_all() {
    notepad.selection_start = 0;
    notepad.selection_end = notepad.text_length - 1;
    draw_content_area();
}

// Simple notepad (backward compatible)
void openNotepadSimple(const char* content) {
    vga_clear_screen(MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    
    uint8_t header_color = MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_WHITE);
    vga_put_string(5, 2, "SCos Notepad", header_color);
    
    if (content) {
        uint8_t text_color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);
        int y = 4;
        int x = 5;
        
        for (int i = 0; content[i] && y < 20; i++) {
            if (content[i] == '\n') {
                y++;
                x = 5;
            } else {
                vga_put_char(x, y, content[i], text_color);
                x++;
                if (x >= 75) {
                    y++;
                    x = 5;
                }
            }
        }
    }
    
    vga_put_string(5, 22, "Use openNotepad() for full editor", MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_WHITE));
}