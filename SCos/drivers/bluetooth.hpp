
#ifndef BLUETOOTH_HPP
#define BLUETOOTH_HPP

#include <stdint.h>

struct BluetoothDevice {
    char name[32];
    uint8_t address[6];
    bool connected;
    int signal_strength;
};

class BluetoothDriver {
public:
    static void init();
    static bool isEnabled();
    static void enable();
    static void disable();
    static int scanDevices(BluetoothDevice* devices, int max_devices);
    static bool connectToDevice(const uint8_t* address);
    static void disconnectDevice(const uint8_t* address);
    static int sendData(const uint8_t* address, const uint8_t* data, int length);
    static bool isPaired(const uint8_t* address);
    static bool pairDevice(const uint8_t* address, const char* pin);
    
private:
    static bool bluetooth_enabled;
    static BluetoothDevice connected_devices[8];
    static int device_count;
};

#endif
