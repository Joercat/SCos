
#include "browser.hpp"
#include "../ui/desktop.hpp"

static int browser_window_id = -1;
static bool browser_visible = false;
static char current_url[MAX_URL_LENGTH] = "scos://home";
static char address_input[MAX_URL_LENGTH] = "";
static char page_content[MAX_PAGE_CONTENT] = "";
static bool address_bar_focused = true;
static int cursor_pos = 0;
static Bookmark bookmarks[MAX_BOOKMARKS];
static HistoryEntry history[MAX_HISTORY];
static int bookmark_count = 0;
static int history_count = 0;
static int current_history_index = -1;

int Browser::custom_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

void Browser::custom_strcpy(char* dest, const char* src) {
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

int Browser::custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

void Browser::init() {
    browser_visible = false;
    custom_strcpy(current_url, "scos://home");
    custom_strcpy(address_input, "");
    address_bar_focused = true;
    cursor_pos = 0;
    bookmark_count = 0;
    history_count = 0;
    current_history_index = -1;
    
    // Add default bookmarks
    addBookmark("scos://home", "SCos Home");
    addBookmark("scos://apps", "App Store");
    addBookmark("scos://settings", "System Settings");
}

void Browser::show() {
    if (browser_visible) return;
    
    browser_window_id = WindowManager::createWindow("SCos Browser", 5, 2, 70, 20);
    if (browser_window_id >= 0) {
        browser_visible = true;
        WindowManager::setActiveWindow(browser_window_id);
        showHomePage();
        drawBrowser();
    }
}

void Browser::hide() {
    if (!browser_visible || browser_window_id < 0) return;
    
    WindowManager::closeWindow(browser_window_id);
    browser_visible = false;
    browser_window_id = -1;
}

bool Browser::isVisible() {
    return browser_visible;
}

void Browser::drawBrowser() {
    if (!browser_visible || browser_window_id < 0) return;
    
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;
    
    drawNavigation();
    drawAddressBar();
    drawContent();
}

void Browser::drawNavigation() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int nav_y = win->y + 1;
    int start_x = win->x + 1;
    
    // Navigation buttons
    const char* nav_text = "[<] [>] [R] [H] [B]";
    for (int i = 0; nav_text[i] && i < win->width - 2; ++i) {
        int idx = 2 * (nav_y * 80 + start_x + i);
        video[idx] = nav_text[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void Browser::drawAddressBar() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int addr_y = win->y + 2;
    int start_x = win->x + 1;
    
    // Address bar label
    const char* label = "URL: ";
    for (int i = 0; label[i]; ++i) {
        int idx = 2 * (addr_y * 80 + start_x + i);
        video[idx] = label[i];
        video[idx + 1] = 0x1F; // Blue
    }
    
    // Address bar content
    int url_start = start_x + 5;
    int url_width = win->width - 7;
    
    for (int i = 0; i < url_width; ++i) {
        int idx = 2 * (addr_y * 80 + url_start + i);
        if (i < custom_strlen(address_bar_focused ? address_input : current_url)) {
            video[idx] = address_bar_focused ? address_input[i] : current_url[i];
        } else {
            video[idx] = ' ';
        }
        video[idx + 1] = address_bar_focused ? 0x4F : 0x17; // Red or grey background
    }
    
    // Cursor
    if (address_bar_focused && cursor_pos < url_width) {
        int idx = 2 * (addr_y * 80 + url_start + cursor_pos);
        video[idx + 1] = 0x4E; // Blinking cursor
    }
}

void Browser::drawContent() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int content_start_y = win->y + 4;
    int content_height = win->height - 5;
    int start_x = win->x + 1;
    
    // Draw page content
    int content_len = custom_strlen(page_content);
    int line = 0;
    int col = 0;
    
    for (int i = 0; i < content_len && line < content_height; ++i) {
        if (page_content[i] == '\n' || col >= win->width - 2) {
            line++;
            col = 0;
            if (page_content[i] == '\n') continue;
        }
        
        if (line < content_height && col < win->width - 2) {
            int idx = 2 * ((content_start_y + line) * 80 + start_x + col);
            video[idx] = page_content[i];
            video[idx + 1] = 0x1E; // Yellow
            col++;
        }
    }
}

void Browser::showHomePage() {
    custom_strcpy(current_url, "scos://home");
    custom_strcpy(page_content, 
        "Welcome to SCos Browser!\n\n"
        "Navigate to:\n"
        "- scos://apps (App Store)\n"
        "- scos://settings (System Settings)\n"
        "- scos://about (About SCos)\n"
        "- scos://help (Help & Documentation)\n\n"
        "Use navigation buttons:\n"
        "[<] Back  [>] Forward  [R] Refresh\n"
        "[H] Home  [B] Bookmarks\n\n"
        "Press Tab to focus address bar\n"
        "Type URL and press Enter to navigate");
    
    addToHistory(current_url, "SCos Home");
}

void Browser::navigateToUrl(const char* url) {
    custom_strcpy(current_url, url);
    addToHistory(url, url);
    
    if (custom_strcmp(url, "scos://home") == 0) {
        showHomePage();
    } else if (custom_strcmp(url, "scos://apps") == 0) {
        custom_strcpy(page_content,
            "SCos App Store\n\n"
            "Available Applications:\n"
            "- Terminal (System terminal)\n"
            "- Notepad (Text editor)\n"
            "- Calculator (Mathematical calculator)\n"
            "- File Manager (File browser)\n"
            "- Calendar (Date and scheduling)\n"
            "- Settings (System configuration)\n"
            "- Security Center (Security tools)\n"
            "- Browser (Web browser)\n\n"
            "Press Alt+Tab to open App Launcher\n"
            "to install and launch applications.");
    } else if (custom_strcmp(url, "scos://settings") == 0) {
        custom_strcpy(page_content,
            "System Settings\n\n"
            "Configuration Options:\n"
            "- Display Settings\n"
            "- Keyboard Layout\n"
            "- Security Settings\n"
            "- Network Configuration\n"
            "- User Accounts\n"
            "- System Information\n\n"
            "Launch Settings app for full configuration.");
    } else if (custom_strcmp(url, "scos://about") == 0) {
        custom_strcpy(page_content,
            "About SCos\n\n"
            "SCos - Simple Computer Operating System\n"
            "Version 1.0\n\n"
            "A lightweight, educational operating system\n"
            "built from scratch in C++.\n\n"
            "Features:\n"
            "- Window management\n"
            "- Application launcher\n"
            "- Built-in applications\n"
            "- File system support\n"
            "- Security features\n\n"
            "Built with love for learning!");
    } else {
        custom_strcpy(page_content,
            "Page not found\n\n"
            "The requested page could not be found.\n"
            "Please check the URL and try again.\n\n"
            "Available pages:\n"
            "- scos://home\n"
            "- scos://apps\n"
            "- scos://settings\n"
            "- scos://about");
    }
}

void Browser::addBookmark(const char* url, const char* title) {
    if (bookmark_count >= MAX_BOOKMARKS) return;
    
    custom_strcpy(bookmarks[bookmark_count].url, url);
    custom_strcpy(bookmarks[bookmark_count].title, title);
    bookmarks[bookmark_count].active = true;
    bookmark_count++;
}

void Browser::addToHistory(const char* url, const char* title) {
    if (history_count >= MAX_HISTORY) {
        // Shift history entries
        for (int i = 0; i < MAX_HISTORY - 1; ++i) {
            history[i] = history[i + 1];
        }
        history_count = MAX_HISTORY - 1;
    }
    
    custom_strcpy(history[history_count].url, url);
    custom_strcpy(history[history_count].title, title);
    history[history_count].active = true;
    current_history_index = history_count;
    history_count++;
}

void Browser::processUrl() {
    navigateToUrl(address_input);
    custom_strcpy(address_input, "");
    cursor_pos = 0;
    address_bar_focused = false;
}

void Browser::handleInput(uint8_t key) {
    if (!browser_visible) return;
    
    switch (key) {
        case 0x0F: // Tab
            address_bar_focused = !address_bar_focused;
            cursor_pos = address_bar_focused ? custom_strlen(address_input) : 0;
            break;
            
        case 0x1C: // Enter
            if (address_bar_focused) {
                processUrl();
            }
            break;
            
        case 0x0E: // Backspace
            if (address_bar_focused && cursor_pos > 0) {
                cursor_pos--;
                address_input[cursor_pos] = '\0';
            }
            break;
            
        case 0x23: // H key - Home
            showHomePage();
            break;
            
        case 0x13: // R key - Refresh
            navigateToUrl(current_url);
            break;
            
        case 0x01: // Escape
            hide();
            break;
            
        default:
            // Handle text input for address bar
            if (address_bar_focused && cursor_pos < MAX_URL_LENGTH - 1) {
                char c = 0;
                // Convert scan code to ASCII (simplified)
                if (key >= 0x02 && key <= 0x0B) { // Numbers 1-0
                    c = '1' + (key - 0x02);
                    if (key == 0x0B) c = '0';
                } else if (key >= 0x10 && key <= 0x19) { // QWERTY row
                    const char qwerty[] = "qwertyuiop";
                    c = qwerty[key - 0x10];
                } else if (key >= 0x1E && key <= 0x26) { // ASDF row
                    const char asdf[] = "asdfghjkl";
                    c = asdf[key - 0x1E];
                } else if (key >= 0x2C && key <= 0x32) { // ZXCV row
                    const char zxcv[] = "zxcvbnm";
                    c = zxcv[key - 0x2C];
                } else if (key == 0x39) { // Space
                    c = ' ';
                } else if (key == 0x34) { // Period
                    c = '.';
                } else if (key == 0x35) { // Slash
                    c = '/';
                } else if (key == 0x27) { // Semicolon (colon)
                    c = ':';
                }
                
                if (c != 0) {
                    address_input[cursor_pos] = c;
                    cursor_pos++;
                    address_input[cursor_pos] = '\0';
                }
            }
            break;
    }
    
    drawBrowser();
}
