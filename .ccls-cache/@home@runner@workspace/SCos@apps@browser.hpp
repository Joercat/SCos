#pragma once
#include "../ui/window_manager.hpp"
#include "html_interpreter.hpp"
#include <stdint.h>

#define MAX_URL_LENGTH 256
#define MAX_PAGE_CONTENT 2048
#define MAX_BOOKMARKS 10
#define MAX_HISTORY 20

struct Bookmark {
    char url[MAX_URL_LENGTH];
    char title[64];
    bool active;
};

struct HistoryEntry {
    char url[MAX_URL_LENGTH];
    char title[64];
    bool active;
};

class Browser {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawBrowser();
    static void drawAddressBar();
    static void drawContent();
    static void drawNavigation();
    static void drawBookmarks();
    static void drawHistory();
    static void navigateToUrl(const char* url);
    static void navigate(const char* url);
    static void refreshPage();
    static void addBookmark(const char* url, const char* title);
    static void addToHistory(const char* url, const char* title);
    static void goBack();
    static void goForward();
    static void refresh();
    static void showHomePage();
    static void processUrl();
    static void loadHTMLFile(const char* filepath);
    static void loadCSSFile(const char* filepath);
    static void loadJSFile(const char* filepath);
    static void renderHTMLPage();
    
};