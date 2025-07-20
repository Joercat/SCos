
#ifndef NETWORK_SETTINGS_HPP
#define NETWORK_SETTINGS_HPP

#include <stdint.h>

class NetworkSettings {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);
    
private:
    static void drawNetworkSettings();
    static void showWifiScan();
    static void showBluetoothScan();
    static void connectToWifi();
    static void connectToBluetooth();
};

#endif
