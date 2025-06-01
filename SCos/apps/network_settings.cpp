
#include "network_settings.hpp"
#include "../ui/window_manager.hpp"
#include "../drivers/network.hpp"
#include "../drivers/bluetooth.hpp"
#include <stdint.h>

// Network Settings state
static int network_window_id = -1;
static bool network_visible = false;
static int selected_option = 0;
static int current_screen = 0; // 0=main, 1=wifi, 2=bluetooth
static bool scanning = false;

void NetworkSettings::init() {
    network_visible = false;
    network_window_id = -1;
    selected_option = 0;
    current_screen = 0;
}

void NetworkSettings::show() {
    if (network_visible) return;

    network_window_id = WindowManager::createWindow("Network Settings", 8, 3, 64, 18);
    if (network_window_id >= 0) {
        network_visible = true;
        WindowManager::setActiveWindow(network_window_id);
        drawNetworkSettings();
    }
}

void NetworkSettings::hide() {
    if (!network_visible || network_window_id < 0) return;

    WindowManager::closeWindow(network_window_id);
    network_visible = false;
    network_window_id = -1;
}

bool NetworkSettings::isVisible() {
    return network_visible;
}

void NetworkSettings::drawNetworkSettings() {
    if (!network_visible || network_window_id < 0) return;

    Window* win = WindowManager::getWindow(network_window_id);
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
        // Main network settings screen
        const char* title = "Network & Bluetooth Settings";
        for (int i = 0; title[i] && i < win->width - 4; ++i) {
            int idx = 2 * (start_y * 80 + start_x + i);
            video[idx] = title[i];
            video[idx + 1] = 0x1E; // Yellow
        }

        // WiFi status
        const char* wifi_status = NetworkDriver::isConnected() ? "Connected" : "Disconnected";
        uint8_t wifi_color = NetworkDriver::isConnected() ? 0x2F : 0x4F;
        
        const char* wifi_label = "WiFi: ";
        int wifi_y = start_y + 2;
        for (int i = 0; wifi_label[i]; ++i) {
            int idx = 2 * (wifi_y * 80 + start_x + i);
            video[idx] = wifi_label[i];
            video[idx + 1] = 0x1F;
        }
        for (int i = 0; wifi_status[i] && i < 20; ++i) {
            int idx = 2 * (wifi_y * 80 + start_x + 6 + i);
            video[idx] = wifi_status[i];
            video[idx + 1] = wifi_color;
        }

        // IP Address
        if (NetworkDriver::isConnected()) {
            const char* ip = NetworkDriver::getIPAddress();
            const char* ip_label = "IP: ";
            int ip_y = wifi_y + 1;
            for (int i = 0; ip_label[i]; ++i) {
                int idx = 2 * (ip_y * 80 + start_x + i);
                video[idx] = ip_label[i];
                video[idx + 1] = 0x1F;
            }
            for (int i = 0; ip[i] && i < 15; ++i) {
                int idx = 2 * (ip_y * 80 + start_x + 4 + i);
                video[idx] = ip[i];
                video[idx + 1] = 0x1F;
            }
        }

        // Bluetooth status
        const char* bt_status = BluetoothDriver::isEnabled() ? "Enabled" : "Disabled";
        uint8_t bt_color = BluetoothDriver::isEnabled() ? 0x2F : 0x4F;
        
        const char* bt_label = "Bluetooth: ";
        int bt_y = start_y + 4;
        for (int i = 0; bt_label[i]; ++i) {
            int idx = 2 * (bt_y * 80 + start_x + i);
            video[idx] = bt_label[i];
            video[idx + 1] = 0x1F;
        }
        for (int i = 0; bt_status[i] && i < 20; ++i) {
            int idx = 2 * (bt_y * 80 + start_x + 11 + i);
            video[idx] = bt_status[i];
            video[idx + 1] = bt_color;
        }

        // Menu options
        const char* menu_items[] = {
            "WiFi Settings",
            "Bluetooth Settings",
            "Network Diagnostics",
            "Toggle WiFi",
            "Toggle Bluetooth"
        };

        for (int i = 0; i < 5; ++i) {
            int option_y = start_y + 7 + i;
            uint8_t color = (i == selected_option) ? 0x70 : 0x1F;
            
            const char* prefix = (i == selected_option) ? "> " : "  ";
            for (int j = 0; prefix[j]; ++j) {
                int idx = 2 * (option_y * 80 + start_x + j);
                video[idx] = prefix[j];
                video[idx + 1] = color;
            }
            
            for (int j = 0; menu_items[i][j] && j < 30; ++j) {
                int idx = 2 * (option_y * 80 + start_x + 2 + j);
                video[idx] = menu_items[i][j];
                video[idx + 1] = color;
            }
        }

        // Instructions
        const char* instructions = "Arrow keys: Navigate | Enter: Select | Esc: Exit";
        int instr_y = win->y + win->height - 2;
        for (int i = 0; instructions[i] && i < win->width - 4; ++i) {
            int idx = 2 * (instr_y * 80 + start_x + i);
            video[idx] = instructions[i];
            video[idx + 1] = 0x08; // Dark grey
        }
    }
}

void NetworkSettings::handleInput(uint8_t key) {
    if (!network_visible) return;

    if (current_screen == 0) {
        switch (key) {
            case 0x48: // Up arrow
                if (selected_option > 0) {
                    selected_option--;
                    drawNetworkSettings();
                }
                break;
            case 0x50: // Down arrow
                if (selected_option < 4) {
                    selected_option++;
                    drawNetworkSettings();
                }
                break;
            case 0x1C: // Enter
                switch (selected_option) {
                    case 0: // WiFi Settings
                        showWifiScan();
                        break;
                    case 1: // Bluetooth Settings
                        showBluetoothScan();
                        break;
                    case 2: // Network Diagnostics
                        // TODO: Implement diagnostics
                        break;
                    case 3: // Toggle WiFi
                        if (NetworkDriver::isConnected()) {
                            NetworkDriver::disconnect();
                        } else {
                            NetworkDriver::connectToWifi("SCos_Network", "password123");
                        }
                        drawNetworkSettings();
                        break;
                    case 4: // Toggle Bluetooth
                        if (BluetoothDriver::isEnabled()) {
                            BluetoothDriver::disable();
                        } else {
                            BluetoothDriver::enable();
                        }
                        drawNetworkSettings();
                        break;
                }
                break;
            case 0x01: // Escape
                hide();
                break;
        }
    }
}

void NetworkSettings::handleMouseClick(int x, int y) {
    if (!network_visible || network_window_id < 0) return;

    Window* win = WindowManager::getWindow(network_window_id);
    if (!win) return;

    // Check if click is within window
    if (x < win->x || x >= win->x + win->width || 
        y < win->y || y >= win->y + win->height) return;

    int start_x = win->x + 2;
    int start_y = win->y + 2;

    // Check if clicked on a menu option
    int rel_y = y - (start_y + 7);
    if (rel_y >= 0 && rel_y < 5) {
        selected_option = rel_y;
        drawNetworkSettings();
    }
}

void NetworkSettings::showWifiScan() {
    // Placeholder for WiFi scanning interface
    current_screen = 1;
    drawNetworkSettings();
}

void NetworkSettings::showBluetoothScan() {
    // Placeholder for Bluetooth scanning interface
    current_screen = 2;
    drawNetworkSettings();
}

void NetworkSettings::connectToWifi() {
    // Placeholder for WiFi connection
}

void NetworkSettings::connectToBluetooth() {
    // Placeholder for Bluetooth connection
}
