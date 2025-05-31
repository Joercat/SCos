#pragma once
#include <stdint.h>

#define MAX_WINDOWS 16
#define MAX_TITLE_LENGTH 32

struct Window {
    int id;
    int x, y;
    int width, height;
    char title[MAX_TITLE_LENGTH];
    bool visible;
    bool focused;
    void* app_data; // Pointer to application-specific data
};

class WindowManager {
public:
    static void init();
    static void clearScreen();
    
    // Window operations
    static int createWindow(const char* title, int x, int y, int width, int height);
    static void drawWindow(int window_id);
    static void moveWindow(int window_id, int new_x, int new_y);
    static void closeWindow(int window_id);
    static void setActiveWindow(int window_id);
    
    // Utility functions
    static void clearWindowArea(int x, int y, int width, int height);
    static Window* getWindow(int window_id);
    static int getActiveWindow();
    static void refreshAll();
    
private:
    static void drawBorder(int x, int y, int width, int height, uint8_t color);
};