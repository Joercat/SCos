#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

#define MAX_STORE_APPS 20
#define MAX_APP_NAME 32
#define MAX_APP_DESC 128
#define MAX_CATEGORIES 8

enum AppCategory {
    CAT_PRODUCTIVITY,
    CAT_UTILITIES,
    CAT_GAMES,
    CAT_DEVELOPMENT,
    CAT_MULTIMEDIA,
    CAT_NETWORK,
    CAT_SYSTEM,
    CAT_EDUCATION
};

struct StoreApp {
    char name[MAX_APP_NAME];
    char description[MAX_APP_DESC];
    char version[16];
    AppCategory category;
    bool installed;
    bool featured;
    int rating; // 1-5 stars
    int downloads;
    void (*install_func)();
};

class AppStore {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawStore();
    static void drawHeader();
    static void drawCategories();
    static void drawAppList();
    static void drawAppDetails();
    static void drawStatusBar();
    static void installApp(int app_index);
    static void uninstallApp(int app_index);
    static void selectNextApp();
    static void selectPrevApp();
    static void selectNextCategory();
    static void selectPrevCategory();
    static void filterByCategory();
    static void searchApps();
    static void updateDisplay();
    static int custom_strlen(const char* str);
    static void custom_strcpy(char* dest, const char* src);
    static int custom_strcmp(const char* str1, const char* str2);
};
#ifndef APP_STORE_HPP
#define APP_STORE_HPP

#include <stdint.h>

class AppStore {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawAppStore();
    static void toggleAppInstallation();
};

#endif
