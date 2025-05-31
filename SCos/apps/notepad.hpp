
#ifndef NOTEPAD_HPP
#define NOTEPAD_HPP

// Function declarations for notepad application
void openNotepad(const char* content);
void openNotepadSimple(const char* content);

// Navigation and editing functions
void notepad_cursor_left();
void notepad_cursor_right();
void notepad_cursor_up();
void notepad_cursor_down();
void notepad_insert_char(char c);
void notepad_backspace();
void notepad_enter();
void notepad_tab();

// File operations
void notepad_new();
void notepad_save();

// Advanced features
void notepad_toggle_word_wrap();
void notepad_select_all();

// Notepad class for input handling
class Notepad {
public:
    static void handleInput(char key);
};

#endif
#ifndef NOTEPAD_HPP
#define NOTEPAD_HPP

#include <stdint.h>

class Notepad {
public:
    static void handleInput(uint8_t key);
};

void openNotepad(const char* content);

#endif
