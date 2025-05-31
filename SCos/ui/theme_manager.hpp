
#pragma once
#include <stdint.h>

enum ThemeType {
    THEME_DEFAULT_BLUE,
    THEME_MATRIX_GREEN,
    THEME_MATRIX_RED,
    THEME_MATRIX_PURPLE,
    THEME_NATURE,
    THEME_COUNT
};

struct Theme {
    const char* name;
    uint8_t background_color;
    uint8_t foreground_color;
    uint8_t window_bg_color;
    uint8_t window_fg_color;
    uint8_t taskbar_bg_color;
    uint8_t taskbar_fg_color;
    uint8_t selected_bg_color;
    uint8_t selected_fg_color;
    uint8_t accent_color;
    bool has_custom_background;
    const char* background_pattern;
};

class ThemeManager {
public:
    static void init();
    static void setTheme(ThemeType theme);
    static ThemeType getCurrentTheme();
    static const Theme& getTheme(ThemeType theme);
    static const Theme& getCurrentThemeData();
    static void drawCustomBackground();
    static void applyThemeColors();
    
    // Background management
    static void setCustomBackground(const char* pattern);
    static void drawNatureBackground();
    static void drawMatrixBackground(uint8_t color);
    
private:
    static void initializeThemes();
};
