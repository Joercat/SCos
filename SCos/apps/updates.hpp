
#ifndef UPDATES_HPP
#define UPDATES_HPP

#include <stdint.h>

struct UpdateInfo {
    const char* component;
    const char* current_version;
    const char* available_version;
    const char* description;
    bool available;
    bool critical;
};

class UpdatesManager {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);
    
private:
    static void drawUpdatesScreen();
    static void checkForUpdates();
    static void installUpdate(int update_index);
    static void showUpdateDetails(int update_index);
    static void downloadUpdate(const char* component);
    static void applyUpdate(const char* component);
};

#endif
