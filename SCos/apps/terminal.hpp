#pragma once
#include "../include/stddef.h"

class Terminal {
private:
    static const int SCREEN_WIDTH = 80;
    static const int SCREEN_HEIGHT = 25;
    static const int MAX_COMMAND_LENGTH = 256;
    static const int HISTORY_SIZE = 10;
    
    volatile char* screen;
    int cursor_x, cursor_y;
    uint8_t current_attr;
    int prompt_x;
    
    char command_buffer[MAX_COMMAND_LENGTH];
    int command_pos;
    
    char* history[HISTORY_SIZE];
    int history_pos;
    
    void clear();
    void put_char(char c);
    void print(const char* str);
    void newline();
    void scroll_up();
    void print_banner();
    void show_prompt();
    
    char scancode_to_char(uint8_t scancode, bool shift);
    void add_to_history(const char* cmd);
    void execute_command(const char* cmd);
    void handle_input();
    
public:
    Terminal();
    ~Terminal();
    void run();
    static void handleInput(uint8_t key);
};

void runTerminal();