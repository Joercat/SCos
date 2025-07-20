
#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

enum SettingsCategory {
    SETTINGS_DISPLAY,
    SETTINGS_AUDIO,
    SETTINGS_NETWORK,
    SETTINGS_SECURITY,
    SETTINGS_SYSTEM
};

class Settings {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawSettings();
    static void drawCategory(SettingsCategory category);
    static void selectCategory(SettingsCategory category);
    static void adjustSetting(int direction);
    static void updateDisplay();
};
