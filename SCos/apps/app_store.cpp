
#include "app_store.hpp"
#include "../ui/desktop.hpp"

static int store_window_id = -1;
static bool store_visible = false;
static StoreApp store_apps[MAX_STORE_APPS];
static int app_count = 0;
static int selected_app = 0;
static AppCategory selected_category = CAT_PRODUCTIVITY;
static bool show_all_categories = true;
static int scroll_offset = 0;

int AppStore::custom_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

void AppStore::custom_strcpy(char* dest, const char* src) {
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

int AppStore::custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

void AppStore::init() {
    store_visible = false;
    app_count = 0;
    selected_app = 0;
    selected_category = CAT_PRODUCTIVITY;
    show_all_categories = true;
    scroll_offset = 0;
    
    // Add sample applications to the store
    StoreApp& terminal_app = store_apps[app_count++];
    custom_strcpy(terminal_app.name, "Enhanced Terminal");
    custom_strcpy(terminal_app.description, "Advanced terminal with scripting support");
    custom_strcpy(terminal_app.version, "2.1");
    terminal_app.category = CAT_DEVELOPMENT;
    terminal_app.installed = true;
    terminal_app.featured = true;
    terminal_app.rating = 5;
    terminal_app.downloads = 1250;
    terminal_app.install_func = nullptr;
    
    StoreApp& text_editor = store_apps[app_count++];
    custom_strcpy(text_editor.name, "Code Editor Pro");
    custom_strcpy(text_editor.description, "Professional code editor with syntax highlighting");
    custom_strcpy(text_editor.version, "1.5");
    text_editor.category = CAT_DEVELOPMENT;
    text_editor.installed = false;
    text_editor.featured = true;
    text_editor.rating = 4;
    text_editor.downloads = 890;
    text_editor.install_func = nullptr;
    
    StoreApp& music_player = store_apps[app_count++];
    custom_strcpy(music_player.name, "Music Player");
    custom_strcpy(music_player.description, "Simple audio player for various formats");
    custom_strcpy(music_player.version, "1.0");
    music_player.category = CAT_MULTIMEDIA;
    music_player.installed = false;
    music_player.featured = false;
    music_player.rating = 3;
    music_player.downloads = 456;
    music_player.install_func = nullptr;
    
    StoreApp& game_snake = store_apps[app_count++];
    custom_strcpy(game_snake.name, "Snake Classic");
    custom_strcpy(game_snake.description, "Classic snake game with modern twists");
    custom_strcpy(game_snake.version, "1.2");
    game_snake.category = CAT_GAMES;
    game_snake.installed = false;
    game_snake.featured = true;
    game_snake.rating = 4;
    game_snake.downloads = 2100;
    game_snake.install_func = nullptr;
    
    StoreApp& network_tool = store_apps[app_count++];
    custom_strcpy(network_tool.name, "Network Monitor");
    custom_strcpy(network_tool.description, "Monitor network connections and traffic");
    custom_strcpy(network_tool.version, "0.8");
    network_tool.category = CAT_NETWORK;
    network_tool.installed = false;
    network_tool.featured = false;
    network_tool.rating = 4;
    network_tool.downloads = 340;
    network_tool.install_func = nullptr;
    
    StoreApp& math_tutor = store_apps[app_count++];
    custom_strcpy(math_tutor.name, "Math Tutor");
    custom_strcpy(math_tutor.description, "Interactive mathematics learning tool");
    custom_strcpy(math_tutor.version, "2.0");
    math_tutor.category = CAT_EDUCATION;
    math_tutor.installed = false;
    math_tutor.featured = true;
    math_tutor.rating = 5;
    math_tutor.downloads = 670;
    math_tutor.install_func = nullptr;
    
    StoreApp& system_cleaner = store_apps[app_count++];
    custom_strcpy(system_cleaner.name, "System Cleaner");
    custom_strcpy(system_cleaner.description, "Clean temporary files and optimize system");
    custom_strcpy(system_cleaner.version, "1.3");
    system_cleaner.category = CAT_SYSTEM;
    system_cleaner.installed = false;
    system_cleaner.featured = false;
    system_cleaner.rating = 3;
    system_cleaner.downloads = 780;
    system_cleaner.install_func = nullptr;
    
    StoreApp& image_viewer = store_apps[app_count++];
    custom_strcpy(image_viewer.name, "Image Viewer");
    custom_strcpy(image_viewer.description, "View and manage image files");
    custom_strcpy(image_viewer.version, "1.1");
    image_viewer.category = CAT_MULTIMEDIA;
    image_viewer.installed = false;
    image_viewer.featured = false;
    image_viewer.rating = 4;
    image_viewer.downloads = 520;
    image_viewer.install_func = nullptr;
}

void AppStore::show() {
    if (store_visible) return;
    
    store_window_id = WindowManager::createWindow("SCos App Store", 3, 1, 74, 22);
    if (store_window_id >= 0) {
        store_visible = true;
        WindowManager::setActiveWindow(store_window_id);
        drawStore();
    }
}

void AppStore::hide() {
    if (!store_visible || store_window_id < 0) return;
    
    WindowManager::closeWindow(store_window_id);
    store_visible = false;
    store_window_id = -1;
}

bool AppStore::isVisible() {
    return store_visible;
}

void AppStore::drawStore() {
    if (!store_visible || store_window_id < 0) return;
    
    drawHeader();
    drawCategories();
    drawAppList();
    drawAppDetails();
    drawStatusBar();
}

void AppStore::drawHeader() {
    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int header_y = win->y + 1;
    int start_x = win->x + 2;
    
    const char* title = "SCos App Store - Discover & Install Applications";
    for (int i = 0; title[i] && i < win->width - 4; ++i) {
        int idx = 2 * (header_y * 80 + start_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x4E; // Red background, yellow text
    }
}

void AppStore::drawCategories() {
    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int cat_y = win->y + 3;
    int start_x = win->x + 2;
    
    const char* categories[] = {
        "All", "Productivity", "Utilities", "Games", 
        "Development", "Multimedia", "Network", "System", "Education"
    };
    
    const char* cat_text = "Categories: ";
    for (int i = 0; cat_text[i]; ++i) {
        int idx = 2 * (cat_y * 80 + start_x + i);
        video[idx] = cat_text[i];
        video[idx + 1] = 0x17; // Grey
    }
    
    int x_offset = start_x + custom_strlen(cat_text);
    for (int i = 0; i < 9; ++i) {
        bool is_selected = (i == 0 && show_all_categories) || 
                          (i > 0 && !show_all_categories && (i - 1) == selected_category);
        uint8_t color = is_selected ? 0x4F : 0x1F; // Red or blue
        
        const char* cat = categories[i];
        for (int j = 0; cat[j] && x_offset < win->x + win->width - 2; ++j) {
            int idx = 2 * (cat_y * 80 + x_offset);
            video[idx] = cat[j];
            video[idx + 1] = color;
            x_offset++;
        }
        
        if (x_offset < win->x + win->width - 2) {
            int idx = 2 * (cat_y * 80 + x_offset);
            video[idx] = ' ';
            video[idx + 1] = 0x17;
            x_offset++;
        }
    }
}

void AppStore::drawAppList() {
    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int list_start_y = win->y + 5;
    int list_height = 10;
    int start_x = win->x + 2;
    
    const char* header = "Name                 Version  Rating  Downloads  Status";
    for (int i = 0; header[i] && i < win->width - 4; ++i) {
        int idx = 2 * (list_start_y * 80 + start_x + i);
        video[idx] = header[i];
        video[idx + 1] = 0x1E; // Yellow
    }
    
    // Draw separator line
    int sep_y = list_start_y + 1;
    for (int i = 0; i < win->width - 4; ++i) {
        int idx = 2 * (sep_y * 80 + start_x + i);
        video[idx] = '-';
        video[idx + 1] = 0x17; // Grey
    }
    
    // Draw app entries
    int visible_apps = 0;
    for (int i = scroll_offset; i < app_count && visible_apps < list_height - 2; ++i) {
        if (!show_all_categories && store_apps[i].category != selected_category) {
            continue;
        }
        
        int entry_y = list_start_y + 2 + visible_apps;
        bool is_selected = (i == selected_app);
        uint8_t color = is_selected ? 0x70 : 0x17; // Inverted or grey
        
        // App name (20 chars)
        for (int j = 0; j < 20; ++j) {
            int idx = 2 * (entry_y * 80 + start_x + j);
            if (j < custom_strlen(store_apps[i].name)) {
                video[idx] = store_apps[i].name[j];
            } else {
                video[idx] = ' ';
            }
            video[idx + 1] = color;
        }
        
        // Version (9 chars)
        for (int j = 0; j < 9; ++j) {
            int idx = 2 * (entry_y * 80 + start_x + 21 + j);
            if (j < custom_strlen(store_apps[i].version)) {
                video[idx] = store_apps[i].version[j];
            } else {
                video[idx] = ' ';
            }
            video[idx + 1] = color;
        }
        
        // Rating (8 chars)
        int rating_start = start_x + 31;
        for (int j = 0; j < store_apps[i].rating; ++j) {
            int idx = 2 * (entry_y * 80 + rating_start + j);
            video[idx] = '*';
            video[idx + 1] = 0x1E; // Yellow stars
        }
        for (int j = store_apps[i].rating; j < 5; ++j) {
            int idx = 2 * (entry_y * 80 + rating_start + j);
            video[idx] = '-';
            video[idx + 1] = color;
        }
        
        // Downloads (11 chars)
        char dl_str[12];
        int dl_len = 0;
        int downloads = store_apps[i].downloads;
        if (downloads == 0) {
            dl_str[0] = '0';
            dl_len = 1;
        } else {
            while (downloads > 0 && dl_len < 10) {
                dl_str[dl_len++] = '0' + (downloads % 10);
                downloads /= 10;
            }
            // Reverse the string
            for (int k = 0; k < dl_len / 2; ++k) {
                char temp = dl_str[k];
                dl_str[k] = dl_str[dl_len - 1 - k];
                dl_str[dl_len - 1 - k] = temp;
            }
        }
        dl_str[dl_len] = '\0';
        
        for (int j = 0; j < 11; ++j) {
            int idx = 2 * (entry_y * 80 + start_x + 40 + j);
            if (j < dl_len) {
                video[idx] = dl_str[j];
            } else {
                video[idx] = ' ';
            }
            video[idx + 1] = color;
        }
        
        // Status (12 chars)
        const char* status = store_apps[i].installed ? "Installed" : "Available";
        for (int j = 0; j < 12; ++j) {
            int idx = 2 * (entry_y * 80 + start_x + 52 + j);
            if (j < custom_strlen(status)) {
                video[idx] = status[j];
            } else {
                video[idx] = ' ';
            }
            video[idx + 1] = store_apps[i].installed ? 0x2F : color;
        }
        
        visible_apps++;
    }
}

void AppStore::drawAppDetails() {
    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;
    
    if (selected_app >= app_count) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int details_y = win->y + 16;
    int start_x = win->x + 2;
    
    const char* details_header = "App Details:";
    for (int i = 0; details_header[i]; ++i) {
        int idx = 2 * (details_y * 80 + start_x + i);
        video[idx] = details_header[i];
        video[idx + 1] = 0x1E; // Yellow
    }
    
    // App description
    StoreApp& app = store_apps[selected_app];
    int desc_y = details_y + 1;
    for (int i = 0; i < custom_strlen(app.description) && i < win->width - 4; ++i) {
        int idx = 2 * (desc_y * 80 + start_x + i);
        video[idx] = app.description[i];
        video[idx + 1] = 0x17; // Grey
    }
    
    // Installation status and actions
    const char* action_text = app.installed ? 
        "Press U to uninstall, I for info" : 
        "Press I to install, Enter for details";
    int action_y = details_y + 2;
    for (int i = 0; i < custom_strlen(action_text) && i < win->width - 4; ++i) {
        int idx = 2 * (action_y * 80 + start_x + i);
        video[idx] = action_text[i];
        video[idx + 1] = 0x1F; // Blue
    }
}

void AppStore::drawStatusBar() {
    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int status_y = win->y + win->height - 2;
    int start_x = win->x + 1;
    
    const char* status = "Use arrows to navigate, C for categories, Esc to exit";
    for (int i = 0; status[i] && i < win->width - 2; ++i) {
        int idx = 2 * (status_y * 80 + start_x + i);
        video[idx] = status[i];
        video[idx + 1] = 0x70; // Black on white
    }
}

void AppStore::installApp(int app_index) {
    if (app_index < 0 || app_index >= app_count) return;
    
    if (!store_apps[app_index].installed) {
        store_apps[app_index].installed = true;
        store_apps[app_index].downloads++;
        // Here you would implement actual installation logic
    }
}

void AppStore::uninstallApp(int app_index) {
    if (app_index < 0 || app_index >= app_count) return;
    
    if (store_apps[app_index].installed) {
        store_apps[app_index].installed = false;
        // Here you would implement actual uninstallation logic
    }
}

void AppStore::selectNextApp() {
    do {
        selected_app = (selected_app + 1) % app_count;
    } while (!show_all_categories && 
             store_apps[selected_app].category != selected_category &&
             selected_app != 0);
}

void AppStore::selectPrevApp() {
    do {
        selected_app = (selected_app - 1 + app_count) % app_count;
    } while (!show_all_categories && 
             store_apps[selected_app].category != selected_category &&
             selected_app != app_count - 1);
}

void AppStore::selectNextCategory() {
    if (show_all_categories) {
        show_all_categories = false;
        selected_category = CAT_PRODUCTIVITY;
    } else {
        selected_category = (AppCategory)((selected_category + 1) % CAT_EDUCATION + 1);
        if (selected_category > CAT_EDUCATION) {
            show_all_categories = true;
        }
    }
    selected_app = 0;
    scroll_offset = 0;
}

void AppStore::selectPrevCategory() {
    if (show_all_categories) {
        show_all_categories = false;
        selected_category = CAT_EDUCATION;
    } else if (selected_category == CAT_PRODUCTIVITY) {
        show_all_categories = true;
    } else {
        selected_category = (AppCategory)(selected_category - 1);
    }
    selected_app = 0;
    scroll_offset = 0;
}

void AppStore::updateDisplay() {
    drawStore();
}

void AppStore::handleInput(uint8_t key) {
    if (!store_visible) return;
    
    switch (key) {
        case 0x48: // Up arrow
            selectPrevApp();
            updateDisplay();
            break;
            
        case 0x50: // Down arrow
            selectNextApp();
            updateDisplay();
            break;
            
        case 0x4B: // Left arrow
            selectPrevCategory();
            updateDisplay();
            break;
            
        case 0x4D: // Right arrow
            selectNextCategory();
            updateDisplay();
            break;
            
        case 0x17: // I key - Install
            installApp(selected_app);
            updateDisplay();
            break;
            
        case 0x16: // U key - Uninstall
            uninstallApp(selected_app);
            updateDisplay();
            break;
            
        case 0x2E: // C key - Categories
            selectNextCategory();
            updateDisplay();
            break;
            
        case 0x1C: // Enter - App details
            // Could open detailed app view
            break;
            
        case 0x01: // Escape
            hide();
            break;
    }
}
