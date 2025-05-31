
#include "theme_manager.hpp"
#include "window_manager.hpp"

static ThemeType current_theme = THEME_DEFAULT_BLUE;
static Theme themes[THEME_COUNT];
static bool themes_initialized = false;

void ThemeManager::init() {
    if (!themes_initialized) {
        initializeThemes();
        themes_initialized = true;
    }
    setTheme(THEME_DEFAULT_BLUE);
}

void ThemeManager::initializeThemes() {
    // Default Blue Theme
    themes[THEME_DEFAULT_BLUE] = {
        "Default Blue",
        0x11,  // Blue background
        0x1F,  // White on blue
        0x1F,  // Blue window background
        0x1E,  // Yellow on blue
        0x70,  // Black on white taskbar
        0x4F,  // White on red accent
        0x4F,  // Red selection
        0x17,  // Grey text
        0x1C,  // Red accent
        false,
        nullptr
    };
    
    // Matrix Green Theme
    themes[THEME_MATRIX_GREEN] = {
        "Matrix Green",
        0x00,  // Black background
        0x0A,  // Bright green on black
        0x02,  // Dark green background
        0x0A,  // Bright green text
        0x20,  // Green on black taskbar
        0x2F,  // White on green accent
        0x2F,  // Green selection
        0x02,  // Dark green
        0x0A,  // Bright green accent
        true,
        "matrix"
    };
    
    // Matrix Red Theme
    themes[THEME_MATRIX_RED] = {
        "Matrix Red",
        0x00,  // Black background
        0x0C,  // Bright red on black
        0x04,  // Dark red background
        0x0C,  // Bright red text
        0x40,  // Red on black taskbar
        0x4F,  // White on red accent
        0x4F,  // Red selection
        0x04,  // Dark red
        0x0C,  // Bright red accent
        true,
        "matrix"
    };
    
    // Matrix Purple Theme
    themes[THEME_MATRIX_PURPLE] = {
        "Matrix Purple",
        0x00,  // Black background
        0x0D,  // Bright magenta on black
        0x05,  // Dark magenta background
        0x0D,  // Bright magenta text
        0x50,  // Magenta on black taskbar
        0x5F,  // White on magenta accent
        0x5F,  // Magenta selection
        0x05,  // Dark magenta
        0x0D,  // Bright magenta accent
        true,
        "matrix"
    };
    
    // Nature Theme
    themes[THEME_NATURE] = {
        "Nature",
        0x02,  // Green background
        0x2F,  // White on green
        0x02,  // Green window background
        0x2A,  // Bright green text
        0x60,  // Brown on green taskbar
        0x6F,  // White on brown accent
        0x3F,  // Cyan selection
        0x2E,  // Yellow on green
        0x0B,  // Cyan accent
        true,
        "nature"
    };
}

void ThemeManager::setTheme(ThemeType theme) {
    if (theme >= THEME_COUNT) return;
    
    current_theme = theme;
    applyThemeColors();
    
    // Clear screen and redraw with new theme
    WindowManager::clearScreen();
    drawCustomBackground();
}

ThemeType ThemeManager::getCurrentTheme() {
    return current_theme;
}

const Theme& ThemeManager::getTheme(ThemeType theme) {
    if (theme >= THEME_COUNT) return themes[THEME_DEFAULT_BLUE];
    return themes[theme];
}

const Theme& ThemeManager::getCurrentThemeData() {
    return themes[current_theme];
}

void ThemeManager::drawCustomBackground() {
    const Theme& theme = getCurrentThemeData();
    
    if (!theme.has_custom_background) {
        // Draw solid color background
        volatile char* video = (volatile char*)0xB8000;
        for (int y = 0; y < 25; ++y) {
            for (int x = 0; x < 80; ++x) {
                int idx = 2 * (y * 80 + x);
                video[idx] = ' ';
                video[idx + 1] = theme.background_color;
            }
        }
        return;
    }
    
    // Draw themed background
    if (theme.background_pattern) {
        if (theme.background_pattern[0] == 'm') { // matrix
            drawMatrixBackground(theme.accent_color);
        } else if (theme.background_pattern[0] == 'n') { // nature
            drawNatureBackground();
        }
    }
}

void ThemeManager::drawMatrixBackground(uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;
    
    // Clear to black first
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x00; // Black background
        }
    }
    
    // Draw matrix-style characters
    const char matrix_chars[] = "01アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン";
    static int offset = 0;
    
    for (int x = 0; x < 80; x += 8) {
        for (int y = 0; y < 25; y += 3) {
            if ((x + y + offset) % 7 == 0) {
                int char_idx = (x + y + offset) % (sizeof(matrix_chars) - 1);
                int idx = 2 * (y * 80 + x);
                if (idx < 4000) {
                    video[idx] = matrix_chars[char_idx];
                    video[idx + 1] = color;
                }
            }
        }
    }
    offset = (offset + 1) % 100;
}

void ThemeManager::drawNatureBackground() {
    volatile char* video = (volatile char*)0xB8000;
    
    // Draw nature-inspired background
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            
            // Create a gradient effect
            if (y < 8) {
                // Sky area
                video[idx] = ' ';
                video[idx + 1] = 0x13; // Cyan background
            } else if (y < 15) {
                // Mountain/forest area
                if ((x + y) % 4 == 0) {
                    video[idx] = '^';
                    video[idx + 1] = 0x2A; // Green
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x22; // Green background
                }
            } else {
                // Water area
                if ((x + y) % 6 == 0) {
                    video[idx] = '~';
                    video[idx + 1] = 0x3B; // Cyan
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x33; // Cyan background
                }
            }
        }
    }
    
    // Add some "trees"
    for (int x = 10; x < 70; x += 15) {
        for (int y = 12; y < 18; ++y) {
            int idx = 2 * (y * 80 + x);
            video[idx] = (y < 15) ? '*' : '|';
            video[idx + 1] = (y < 15) ? 0x2A : 0x6E; // Green or brown
        }
    }
}

void ThemeManager::applyThemeColors() {
    // This would be called by other UI components to get theme colors
    // The actual application of colors happens in the drawing functions
}

void ThemeManager::setCustomBackground(const char* pattern) {
    themes[current_theme].background_pattern = pattern;
    themes[current_theme].has_custom_background = true;
    drawCustomBackground();
}
