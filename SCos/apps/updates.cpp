
#include "updates.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Updates state
static int updates_window_id = -1;
static bool updates_visible = false;
static int selected_update = 0;
static int current_screen = 0; // 0=main, 1=details, 2=installing
static bool checking_updates = false;

// Mock update data
static UpdateInfo available_updates[] = {
    {"Kernel", "v1.2.3", "v1.2.4", "Security patches and performance improvements", true, true},
    {"Window Manager", "v2.1.0", "v2.1.1", "Bug fixes for window positioning", true, false},
    {"File System", "v1.0.5", "v1.0.6", "Enhanced file operations support", true, false},
    {"Security Module", "v1.1.2", "v1.1.3", "Updated authentication system", true, true},
    {"UI Framework", "v3.0.1", "v3.0.2", "Visual improvements and theme updates", true, false},
    {"Device Drivers", "v1.0.8", "v1.0.9", "Better hardware compatibility", false, false}
};

static const int update_count = 6;
static char install_progress[64];
static int install_percent = 0;

void UpdatesManager::init() {
    updates_visible = false;
    updates_window_id = -1;
    selected_update = 0;
    current_screen = 0;
}

void UpdatesManager::show() {
    if (updates_visible) return;

    updates_window_id = WindowManager::createWindow("System Updates", 10, 2, 60, 20);
    if (updates_window_id >= 0) {
        updates_visible = true;
        WindowManager::setActiveWindow(updates_window_id);
        checkForUpdates();
        drawUpdatesScreen();
    }
}

void UpdatesManager::hide() {
    if (!updates_visible || updates_window_id < 0) return;

    WindowManager::closeWindow(updates_window_id);
    updates_visible = false;
    updates_window_id = -1;
}

bool UpdatesManager::isVisible() {
    return updates_visible;
}

void UpdatesManager::drawUpdatesScreen() {
    if (!updates_visible || updates_window_id < 0) return;

    Window* win = WindowManager::getWindow(updates_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int start_x = win->x + 2;
    int start_y = win->y + 2;

    // Clear content area
    for (int y = start_y; y < win->y + win->height - 1; ++y) {
        for (int x = start_x; x < win->x + win->width - 2; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x1F; // Blue background
        }
    }

    if (current_screen == 0) {
        // Main updates screen
        const char* title = "System Updates Available";
        for (int i = 0; title[i] && i < win->width - 4; ++i) {
            int idx = 2 * (start_y * 80 + start_x + i);
            video[idx] = title[i];
            video[idx + 1] = 0x1E; // Yellow
        }

        // Status
        int available_count = 0;
        int critical_count = 0;
        for (int i = 0; i < update_count; ++i) {
            if (available_updates[i].available) {
                available_count++;
                if (available_updates[i].critical) critical_count++;
            }
        }

        char status_text[64];
        // Simple string building
        const char* prefix = "Available: ";
        int pos = 0;
        while (prefix[pos]) {
            status_text[pos] = prefix[pos];
            pos++;
        }
        status_text[pos++] = '0' + available_count;
        status_text[pos++] = ' ';
        status_text[pos++] = '|';
        status_text[pos++] = ' ';
        const char* suffix = "Critical: ";
        for (int i = 0; suffix[i]; ++i) {
            status_text[pos++] = suffix[i];
        }
        status_text[pos++] = '0' + critical_count;
        status_text[pos] = '\0';

        int status_y = start_y + 2;
        for (int i = 0; status_text[i] && i < win->width - 4; ++i) {
            int idx = 2 * (status_y * 80 + start_x + i);
            video[idx] = status_text[i];
            video[idx + 1] = critical_count > 0 ? 0x4F : 0x2F; // Red if critical, green otherwise
        }

        // Update list
        for (int i = 0; i < update_count && i < win->height - 8; ++i) {
            if (!available_updates[i].available) continue;
            
            int update_y = start_y + 4 + i;
            uint8_t color = (i == selected_update) ? 0x70 : 0x1F; // White on black if selected

            // Component name
            for (int j = 0; available_updates[i].component[j] && j < 15; ++j) {
                int idx = 2 * (update_y * 80 + start_x + j);
                video[idx] = available_updates[i].component[j];
                video[idx + 1] = color;
            }

            // Version info
            int ver_x = start_x + 16;
            for (int j = 0; available_updates[i].current_version[j] && j < 8; ++j) {
                int idx = 2 * (update_y * 80 + ver_x + j);
                video[idx] = available_updates[i].current_version[j];
                video[idx + 1] = color;
            }

            // Arrow
            int arrow_x = ver_x + 9;
            int idx = 2 * (update_y * 80 + arrow_x);
            video[idx] = '-';
            video[idx + 1] = color;
            idx = 2 * (update_y * 80 + arrow_x + 1);
            video[idx] = '>';
            video[idx + 1] = color;

            // New version
            int new_ver_x = arrow_x + 3;
            for (int j = 0; available_updates[i].available_version[j] && j < 8; ++j) {
                int idx = 2 * (update_y * 80 + new_ver_x + j);
                video[idx] = available_updates[i].available_version[j];
                video[idx + 1] = color;
            }

            // Critical indicator
            if (available_updates[i].critical) {
                int crit_x = new_ver_x + 10;
                const char* crit_text = "[CRITICAL]";
                for (int j = 0; crit_text[j] && j < 10; ++j) {
                    int idx = 2 * (update_y * 80 + crit_x + j);
                    video[idx] = crit_text[j];
                    video[idx + 1] = 0x4F; // Red
                }
            }
        }

        // Instructions
        const char* instructions[] = {
            "Arrow keys: Select update",
            "Enter: Install selected | Space: Details | A: Install All",
            "C: Check for updates | Esc: Exit"
        };

        for (int i = 0; i < 3; ++i) {
            int instr_y = win->y + win->height - 4 + i;
            for (int j = 0; instructions[i][j] && j < win->width - 4; ++j) {
                int idx = 2 * (instr_y * 80 + start_x + j);
                video[idx] = instructions[i][j];
                video[idx + 1] = 0x08; // Dark grey
            }
        }

    } else if (current_screen == 2) {
        // Installation screen
        const char* title = "Installing Updates...";
        for (int i = 0; title[i] && i < win->width - 4; ++i) {
            int idx = 2 * (start_y * 80 + start_x + i);
            video[idx] = title[i];
            video[idx + 1] = 0x1E; // Yellow
        }

        // Progress bar
        int progress_y = start_y + 4;
        int progress_width = 40;
        int filled = (install_percent * progress_width) / 100;

        // Progress bar border
        int idx = 2 * (progress_y * 80 + start_x);
        video[idx] = '[';
        video[idx + 1] = 0x1F;
        
        for (int i = 0; i < progress_width; ++i) {
            idx = 2 * (progress_y * 80 + start_x + 1 + i);
            video[idx] = (i < filled) ? '#' : ' ';
            video[idx + 1] = (i < filled) ? 0x2F : 0x1F;
        }
        
        idx = 2 * (progress_y * 80 + start_x + progress_width + 1);
        video[idx] = ']';
        video[idx + 1] = 0x1F;

        // Percentage
        char percent_text[8];
        percent_text[0] = ' ';
        percent_text[1] = '0' + (install_percent / 10);
        percent_text[2] = '0' + (install_percent % 10);
        percent_text[3] = '%';
        percent_text[4] = '\0';
        
        for (int i = 0; percent_text[i]; ++i) {
            idx = 2 * (progress_y * 80 + start_x + progress_width + 3 + i);
            video[idx] = percent_text[i];
            video[idx + 1] = 0x1F;
        }

        // Progress text
        for (int i = 0; install_progress[i] && i < win->width - 4; ++i) {
            idx = 2 * ((progress_y + 2) * 80 + start_x + i);
            video[idx] = install_progress[i];
            video[idx + 1] = 0x1F;
        }
    }
}

void UpdatesManager::checkForUpdates() {
    checking_updates = true;
    // Simulate checking - in real implementation this would contact update servers
    
    // Mock: randomly make some updates available
    for (int i = 0; i < update_count; ++i) {
        available_updates[i].available = (i % 2 == 0); // Make every other update available
    }
    
    checking_updates = false;
}

void UpdatesManager::installUpdate(int update_index) {
    if (update_index < 0 || update_index >= update_count) return;
    
    current_screen = 2;
    install_percent = 0;
    
    // Simulate installation process
    const char* component = available_updates[update_index].component;
    
    // Copy "Installing " to install_progress
    const char* prefix = "Installing ";
    int pos = 0;
    while (prefix[pos]) {
        install_progress[pos] = prefix[pos];
        pos++;
    }
    
    // Copy component name
    for (int i = 0; component[i] && pos < 50; ++i) {
        install_progress[pos++] = component[i];
    }
    
    // Add "..."
    install_progress[pos++] = '.';
    install_progress[pos++] = '.';
    install_progress[pos++] = '.';
    install_progress[pos] = '\0';
    
    drawUpdatesScreen();
    
    // Simulate progress (in real implementation this would be actual installation)
    for (int i = 0; i <= 100; i += 10) {
        install_percent = i;
        drawUpdatesScreen();
        
        // Simple delay simulation
        for (volatile int j = 0; j < 1000000; ++j) { }
    }
    
    // Mark as no longer available (installed)
    available_updates[update_index].available = false;
    
    // Return to main screen
    current_screen = 0;
    drawUpdatesScreen();
}

void UpdatesManager::handleInput(uint8_t key) {
    if (!updates_visible) return;

    if (current_screen == 0) {
        switch (key) {
            case 0x48: // Up arrow
                if (selected_update > 0) {
                    selected_update--;
                    drawUpdatesScreen();
                }
                break;
            case 0x50: // Down arrow
                if (selected_update < update_count - 1) {
                    selected_update++;
                    drawUpdatesScreen();
                }
                break;
            case 0x1C: // Enter - Install selected
                if (available_updates[selected_update].available) {
                    installUpdate(selected_update);
                }
                break;
            case 0x39: // Space - Show details
                showUpdateDetails(selected_update);
                break;
            case 0x1E: // A - Install all
                for (int i = 0; i < update_count; ++i) {
                    if (available_updates[i].available) {
                        installUpdate(i);
                    }
                }
                break;
            case 0x2E: // C - Check for updates
                checkForUpdates();
                drawUpdatesScreen();
                break;
            case 0x01: // Escape
                hide();
                break;
        }
    } else if (current_screen == 2) {
        // During installation, only allow escape
        if (key == 0x01) {
            current_screen = 0;
            drawUpdatesScreen();
        }
    }
}

void UpdatesManager::handleMouseClick(int x, int y) {
    if (!updates_visible || updates_window_id < 0) return;

    Window* win = WindowManager::getWindow(updates_window_id);
    if (!win) return;

    // Check if click is within window
    if (x < win->x || x >= win->x + win->width || 
        y < win->y || y >= win->y + win->height) return;

    int start_x = win->x + 2;
    int start_y = win->y + 2;

    if (current_screen == 0) {
        // Check if clicked on an update
        int rel_y = y - (start_y + 4);
        if (rel_y >= 0 && rel_y < update_count && available_updates[rel_y].available) {
            selected_update = rel_y;
            drawUpdatesScreen();
        }
    }
}

void UpdatesManager::showUpdateDetails(int update_index) {
    // For now, just flash the selection - could expand to show detailed window
    drawUpdatesScreen();
}

void UpdatesManager::downloadUpdate(const char* component) {
    // Placeholder for download functionality
}

void UpdatesManager::applyUpdate(const char* component) {
    // Placeholder for applying updates
}
