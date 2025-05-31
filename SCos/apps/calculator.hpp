#ifndef CALCULATOR_HPP
#define CALCULATOR_HPP

#include <stdint.h>

class Calculator {
public:
    static void init();
    static void run();
    static void handleInput(uint8_t key);
    static void draw();
    static void clear();
    static void calculate();

private:
    static void drawDisplay();
    static void drawButtons();
    static void processNumber(char digit);
    static void processOperator(char op);
    static void processEquals();
    static void processClear();

    static char display[32];
    static double current_value;
    static double stored_value;
    static char current_operator;
    static bool new_number;
    static int window_id;
};

// Standalone function for desktop integration
void launchCalculator();
void handleCalculatorInput(uint8_t key);
void closeCalculator();
void drawCalculator();
void inputDigit(int digit);
void inputOperator(char op);
void calculateResult();
void clearCalculator();
bool isCalculatorVisible();

#endif