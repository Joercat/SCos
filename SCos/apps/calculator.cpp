
#include "calculator.hpp"
#include "../ui/window_manager.hpp"
#include "../include/string.h"
#include <stdint.h>

// Calculator state
static char display[32] = "0";
static double current_value = 0.0;
static double stored_value = 0.0;
static char current_operator = '\0';
static bool new_number = true;
static int calculator_window_id = -1;

// Simple string to double conversion
static double str_to_double(const char* str) {
    double result = 0.0;
    double fraction = 0.0;
    bool negative = false;
    bool decimal_part = false;
    double divisor = 10.0;
    
    if (*str == '-') {
        negative = true;
        str++;
    }
    
    while (*str) {
        if (*str == '.') {
            decimal_part = true;
        } else if (*str >= '0' && *str <= '9') {
            if (decimal_part) {
                fraction += (*str - '0') / divisor;
                divisor *= 10.0;
            } else {
                result = result * 10.0 + (*str - '0');
            }
        }
        str++;
    }
    
    result += fraction;
    return negative ? -result : result;
}

// Simple double to string conversion
static void double_to_str(double value, char* buffer, int buffer_size) {
    if (buffer_size < 2) return;
    
    int int_part = (int)value;
    double frac_part = value - int_part;
    
    // Handle negative numbers
    int pos = 0;
    if (value < 0) {
        buffer[pos++] = '-';
        int_part = -int_part;
        frac_part = -frac_part;
    }
    
    // Convert integer part
    if (int_part == 0) {
        buffer[pos++] = '0';
    } else {
        char temp[16];
        int temp_pos = 0;
        while (int_part > 0 && temp_pos < 15) {
            temp[temp_pos++] = '0' + (int_part % 10);
            int_part /= 10;
        }
        
        // Reverse the digits
        for (int i = temp_pos - 1; i >= 0 && pos < buffer_size - 1; i--) {
            buffer[pos++] = temp[i];
        }
    }
    
    // Add decimal part if significant
    if (frac_part > 0.001 && pos < buffer_size - 3) {
        buffer[pos++] = '.';
        int decimal_places = 2;
        while (decimal_places > 0 && pos < buffer_size - 1) {
            frac_part *= 10;
            int digit = (int)frac_part;
            buffer[pos++] = '0' + digit;
            frac_part -= digit;
            decimal_places--;
        }
    }
    
    buffer[pos] = '\0';
}

void Calculator::init() {
    strcpy(display, "0");
    current_value = 0.0;
    stored_value = 0.0;
    current_operator = '\0';
    new_number = true;
    calculator_window_id = -1;
}

void Calculator::run() {
    calculator_window_id = WindowManager::createWindow("Calculator", 30, 8, 25, 12);
    if (calculator_window_id >= 0) {
        WindowManager::setActiveWindow(calculator_window_id);
        draw();
    }
}

void Calculator::draw() {
    if (calculator_window_id < 0) return;
    
    Window* win = WindowManager::getWindow(calculator_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    
    // Draw display
    int display_y = win->y + 2;
    int display_x = win->x + 2;
    
    // Clear display area
    for (int i = 0; i < 20; ++i) {
        int idx = 2 * (display_y * 80 + display_x + i);
        video[idx] = ' ';
        video[idx + 1] = 0x70; // Black on white
    }
    
    // Show current display value
    int len = strlen(display);
    for (int i = 0; i < len && i < 20; ++i) {
        int idx = 2 * (display_y * 80 + display_x + i);
        video[idx] = display[i];
        video[idx + 1] = 0x70; // Black on white
    }
    
    // Draw buttons
    const char* buttons[4][4] = {
        {"7", "8", "9", "/"},
        {"4", "5", "6", "*"},
        {"1", "2", "3", "-"},
        {"0", ".", "=", "+"}
    };
    
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            int btn_x = win->x + 2 + col * 5;
            int btn_y = win->y + 4 + row * 2;
            
            if (btn_y < win->y + win->height - 1) {
                int idx = 2 * (btn_y * 80 + btn_x);
                video[idx] = buttons[row][col][0];
                video[idx + 1] = 0x1F; // White on blue
            }
        }
    }
    
    // Instructions
    const char* instructions = "Use number keys and operators";
    int instr_y = win->y + win->height - 2;
    for (int i = 0; instructions[i] && i < 22; ++i) {
        int idx = 2 * (instr_y * 80 + win->x + 2 + i);
        video[idx] = instructions[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void Calculator::handleInput(uint8_t key) {
    if (calculator_window_id < 0) return;
    
    // Handle number keys
    if (key >= 0x02 && key <= 0x0A) { // 1-9
        processNumber('1' + (key - 0x02));
    } else if (key == 0x0B) { // 0
        processNumber('0');
    } else if (key == 0x34) { // . (period)
        processNumber('.');
    }
    // Handle operators
    else if (key == 0x35) { // / (forward slash)
        processOperator('/');
    } else if (key == 0x37) { // * (asterisk)
        processOperator('*');
    } else if (key == 0x4A) { // - (minus)
        processOperator('-');
    } else if (key == 0x4E) { // + (plus)
        processOperator('+');
    } else if (key == 0x1C) { // Enter (equals)
        processEquals();
    } else if (key == 0x0E) { // Backspace (clear)
        processClear();
    } else if (key == 0x01) { // Escape (close)
        WindowManager::closeWindow(calculator_window_id);
        calculator_window_id = -1;
        return;
    }
    
    draw();
}

void Calculator::processNumber(char digit) {
    if (new_number) {
        if (digit == '.') {
            strcpy(display, "0.");
        } else {
            display[0] = digit;
            display[1] = '\0';
        }
        new_number = false;
    } else {
        int len = strlen(display);
        if (len < 30) {
            display[len] = digit;
            display[len + 1] = '\0';
        }
    }
}

void Calculator::processOperator(char op) {
    if (!new_number) {
        current_value = str_to_double(display);
    }
    
    if (current_operator != '\0' && !new_number) {
        processEquals();
    }
    
    stored_value = current_value;
    current_operator = op;
    new_number = true;
}

void Calculator::processEquals() {
    if (current_operator == '\0') return;
    
    double operand = str_to_double(display);
    
    switch (current_operator) {
        case '+':
            current_value = stored_value + operand;
            break;
        case '-':
            current_value = stored_value - operand;
            break;
        case '*':
            current_value = stored_value * operand;
            break;
        case '/':
            if (operand != 0.0) {
                current_value = stored_value / operand;
            } else {
                strcpy(display, "Error");
                new_number = true;
                current_operator = '\0';
                return;
            }
            break;
    }
    
    double_to_str(current_value, display, sizeof(display));
    current_operator = '\0';
    new_number = true;
}

void Calculator::processClear() {
    strcpy(display, "0");
    current_value = 0.0;
    stored_value = 0.0;
    current_operator = '\0';
    new_number = true;
}

void Calculator::clear() {
    processClear();
    if (calculator_window_id >= 0) {
        draw();
    }
}

void Calculator::calculate() {
    processEquals();
    if (calculator_window_id >= 0) {
        draw();
    }
}

// Standalone function for desktop integration
void launchCalculator() {
    Calculator::init();
    Calculator::run();
}
