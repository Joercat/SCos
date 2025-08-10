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
    themes[THEME_DEFAULT_BLUE] = {
        "Default Blue",
        0x11,
        0x1F,
        0x1F,
        0x1E,
        0x70,
        0x4F,
        0x4F,
        0x17,
        0x1C,
        false,
        nullptr
    };

    themes[THEME_MATRIX_GREEN] = {
        "Matrix Green",
        0x00,
        0x0A,
        0x00,
        0x0A,
        0x00,
        0x0A,
        0x0A,
        0x0A,
        0x0A,
        true,
        "matrix"
    };

    themes[THEME_MATRIX_RED] = {
        "Matrix Red",
        0x00,
        0x0C,
        0x04,
        0x0C,
        0x40,
        0x4F,
        0x4F,
        0x04,
        0x0C,
        true,
        "matrix"
    };

    themes[THEME_MATRIX_PURPLE] = {
        "Matrix Purple",
        0x00,
        0x0D,
        0x05,
        0x0D,
        0x50,
        0x5F,
        0x5F,
        0x05,
        0x0D,
        true,
        "matrix"
    };

    themes[THEME_NATURE] = {
        "Nature",
        0x02,
        0x2F,
        0x02,
        0x2A,
        0x60,
        0x6F,
        0x3F,
        0x2E,
        0x0B,
        true,
        "nature"
    };
}

void ThemeManager::setTheme(ThemeType theme) {
    if (theme >= THEME_COUNT) return;

    current_theme = theme;
    applyThemeColors();

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

    if (theme.background_pattern) {
        if (theme.background_pattern[0] == 'm') {
            drawMatrixBackground(theme.accent_color);
        } else if (theme.background_pattern[0] == 'n') {
            drawNatureBackgroundFromFile();
        }
    }
}

void ThemeManager::drawMatrixBackground(uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;

    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x00;
        }
    }

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

    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);

            if (y < 10) {
                video[idx] = ' ';
                video[idx + 1] = 0x9F;
            } else {
                video[idx] = (x + y) % 3 == 0 ? '.' : ' ';
                video[idx + 1] = (y < 15) ? 0x2A : 0x6E;
            }
        }
    }
}

void ThemeManager::drawNatureBackgroundFromFile() {
    const char* background_data = loadBackgroundImage("../attached_assets/SCos-background.jpg");

    if (background_data) {
        drawImageAsASCII(background_data);
    } else {
        drawNatureBackground();
    }
}

const char* ThemeManager::loadBackgroundImage(const char* path) {
    static const char nature_pattern[] = 
        "                    Waterfall Scene                     "
        "        ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~            "
        "      ~~~  .*.   Trees and Moss   .*.  ~~~             "
        "    ~~~   .*.*. Flowing Water  .*.* ~~~              "
        "   ~~  .*.*.*.  ||||||||||||  .*.*.*.  ~~              "
        "  ~~ .*.*.*.*. |||||||||||| .*.*.*.*. ~~               "
        " ~  .*.*.*.*.  ||||||||||||  .*.*.*.* ~                "
        "~  .*.*.*.*.* |||||||||||| .*.*.*.*.* ~              ";

    return nature_pattern;
}

void ThemeManager::drawImageAsASCII(const char* image_data) {
    volatile char* video = (volatile char*)0xB8000;

    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);

            if (y < 5) {
                video[idx] = ' ';
                video[idx + 1] = 0x1F;
            } else if (y < 15 && x > 30 && x < 50) {
                video[idx] = (y + x) % 2 == 0 ? '|' : ' ';
                video[idx + 1] = 0x3F;
            } else if (y >= 15) {
                if ((x + y) % 4 == 0) {
                    video[idx] = 'o';
                    video[idx + 1] = 0x60;
                } else if ((x + y) % 3 == 0) {
                    video[idx] = '*';
                    video[idx + 1] = 0x2A;
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x20;
                }
            } else {
                if ((x + y) % 5 == 0) {
                    video[idx] = '*';
                    video[idx + 1] = 0x2A;
                } else if (x % 8 == 3 && y > 8) {
                    video[idx] = '|';
                    video[idx + 1] = 0x64;
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x20;
                }
            }
        }
    }
}

void ThemeManager::applyThemeColors() {
}

void ThemeManager::setCustomBackground(const char* pattern) {
    themes[current_theme].background_pattern = pattern;
    themes[current_theme].has_custom_background = true;
    drawCustomBackground();
}
