#include "theme_manager.hpp"
#include "window_manager.hpp"

static ThemeType current_theme = THEME_MATRIX_GREEN;
static Theme themes[THEME_COUNT];
static bool themes_initialized = false;

void ThemeManager::init() {
    if (!themes_initialized) {
        initializeThemes();
        themes_initialized = true;
    }
    setTheme(THEME_MATRIX_GREEN);
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

    // Matrix Green Theme (#39ff14 on black)
    themes[THEME_MATRIX_GREEN] = {
        "Matrix Green",
        0x00,  // Black background
        0x0A,  // Bright green (#39ff14) on black
        0x00,  // Black window background
        0x0A,  // Bright green text
        0x00,  // Black taskbar background
        0x0A,  // Bright green on black
        0x0A,  // Green selection
        0x0A,  // Bright green
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
            drawNatureBackgroundFromFile();
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

    // Draw a simple nature scene with ASCII
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);

            // Sky and ground
            if (y < 10) {
                video[idx] = ' ';
                video[idx + 1] = 0x9F; // Light blue background
            } else {
                video[idx] = (x + y) % 3 == 0 ? '.' : ' ';
                video[idx + 1] = (y < 15) ? 0x2A : 0x6E; // Green or brown
            }
        }
    }
}

void ThemeManager::drawNatureBackgroundFromFile() {
    // Try to load the background image from the file system
    const char* background_data = loadBackgroundImage("../attached_assets/SCos-background.jpg");

    if (background_data) {
        drawImageAsASCII(background_data);
    } else {
        // Fallback to ASCII nature background
        drawNatureBackground();
    }
}

const char* ThemeManager::loadBackgroundImage(const char* path) {
    // In a real OS, this would read the image file
    // For now, we'll simulate reading the image and convert to ASCII representation

    // Since we can't actually load JPEG in this simple OS, 
    // we'll create a nature-themed ASCII pattern based on the image concept
    static const char nature_pattern[] = 
        "                    Waterfall Scene                     "
        "        ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~            "
        "      ~~~  .*.   Trees and Moss   .*.  ~~~             "
        "    ~~~   .*.*. Flowing Water  .*.*   ~~~              "
        "   ~~  .*.*.*.  ||||||||||||  .*.*.*.  ~~              "
        "  ~~ .*.*.*.*. |||||||||||| .*.*.*.*. ~~               "
        " ~  .*.*.*.*.  ||||||||||||  .*.*.*.* ~                "
        "~  .*.*.*.*.*  |||||||||||| .*.*.*.*.*  ~              ";

    return nature_pattern;
}

void ThemeManager::drawImageAsASCII(const char* image_data) {
    volatile char* video = (volatile char*)0xB8000;

    // Convert the "image data" to a beautiful ASCII representation
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);

            // Create a waterfall/nature scene
            if (y < 5) {
                // Sky
                video[idx] = ' ';
                video[idx + 1] = 0x1F; // Blue background
            } else if (y < 15 && x > 30 && x < 50) {
                // Waterfall
                video[idx] = (y + x) % 2 == 0 ? '|' : ' ';
                video[idx + 1] = 0x3F; // Cyan on blue
            } else if (y >= 15) {
                // Rocks and moss at bottom
                if ((x + y) % 4 == 0) {
                    video[idx] = 'o'; // Rocks
                    video[idx + 1] = 0x60; // Brown
                } else if ((x + y) % 3 == 0) {
                    video[idx] = '*'; // Moss
                    video[idx + 1] = 0x2A; // Green
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x20; // Green background
                }
            } else {
                // Trees and foliage
                if ((x + y) % 5 == 0) {
                    video[idx] = '*'; // Leaves
                    video[idx + 1] = 0x2A; // Green
                } else if (x % 8 == 3 && y > 8) {
                    video[idx] = '|'; // Tree trunk
                    video[idx + 1] = 0x64; // Brown
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x20; // Green background
                }
            }
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