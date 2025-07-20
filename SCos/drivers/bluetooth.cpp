
#include "bluetooth.hpp"
#include "../debug/serial.hpp"

// Static member definitions
bool BluetoothDriver::bluetooth_enabled = false;
BluetoothDevice BluetoothDriver::connected_devices[8];
int BluetoothDriver::device_count = 0;

// Helper functions
static void safe_strcpy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void copy_address(uint8_t* dest, const uint8_t* src) {
    for (int i = 0; i < 6; i++) {
        dest[i] = src[i];
    }
}

void BluetoothDriver::init() {
    serial_printf("Initializing Bluetooth driver...\n");
    bluetooth_enabled = false;
    device_count = 0;
    
    // Clear connected devices
    for (int i = 0; i < 8; i++) {
        connected_devices[i].name[0] = '\0';
        connected_devices[i].connected = false;
        connected_devices[i].signal_strength = 0;
        for (int j = 0; j < 6; j++) {
            connected_devices[i].address[j] = 0;
        }
    }
    
    serial_printf("Bluetooth driver initialized\n");
}

bool BluetoothDriver::isEnabled() {
    return bluetooth_enabled;
}

void BluetoothDriver::enable() {
    serial_printf("Enabling Bluetooth...\n");
    bluetooth_enabled = true;
    serial_printf("Bluetooth enabled\n");
}

void BluetoothDriver::disable() {
    if (bluetooth_enabled) {
        serial_printf("Disabling Bluetooth...\n");
        bluetooth_enabled = false;
        device_count = 0;
        serial_printf("Bluetooth disabled\n");
    }
}

int BluetoothDriver::scanDevices(BluetoothDevice* devices, int max_devices) {
    if (!bluetooth_enabled) return 0;
    
    serial_printf("Scanning for Bluetooth devices...\n");
    
    // Mock some discovered devices
    const char* mock_names[] = {
        "iPhone 15",
        "Galaxy Buds",
        "MacBook Pro",
        "Wireless Mouse"
    };
    
    uint8_t mock_addresses[][6] = {
        {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC},
        {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56},
        {0x98, 0x76, 0x54, 0x32, 0x10, 0xFE},
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
    };
    
    int found_count = 4;
    if (found_count > max_devices) found_count = max_devices;
    
    for (int i = 0; i < found_count; i++) {
        safe_strcpy(devices[i].name, mock_names[i], 32);
        copy_address(devices[i].address, mock_addresses[i]);
        devices[i].connected = false;
        devices[i].signal_strength = 75 - (i * 10); // Mock signal strength
    }
    
    serial_printf("Found %d Bluetooth devices\n", found_count);
    return found_count;
}

bool BluetoothDriver::connectToDevice(const uint8_t* address) {
    if (!bluetooth_enabled) return false;
    
    serial_printf("Connecting to Bluetooth device...\n");
    
    // Mock successful connection
    if (device_count < 8) {
        copy_address(connected_devices[device_count].address, address);
        safe_strcpy(connected_devices[device_count].name, "Connected Device", 32);
        connected_devices[device_count].connected = true;
        connected_devices[device_count].signal_strength = 80;
        device_count++;
        
        serial_printf("Bluetooth device connected\n");
        return true;
    }
    
    return false;
}

void BluetoothDriver::disconnectDevice(const uint8_t* address) {
    for (int i = 0; i < device_count; i++) {
        bool match = true;
        for (int j = 0; j < 6; j++) {
            if (connected_devices[i].address[j] != address[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            serial_printf("Disconnecting Bluetooth device: %s\n", connected_devices[i].name);
            
            // Shift devices down
            for (int k = i; k < device_count - 1; k++) {
                connected_devices[k] = connected_devices[k + 1];
            }
            device_count--;
            break;
        }
    }
}

int BluetoothDriver::sendData(const uint8_t* address, const uint8_t* data, int length) {
    if (!bluetooth_enabled) return -1;
    
    // Check if device is connected
    for (int i = 0; i < device_count; i++) {
        bool match = true;
        for (int j = 0; j < 6; j++) {
            if (connected_devices[i].address[j] != address[j]) {
                match = false;
                break;
            }
        }
        
        if (match && connected_devices[i].connected) {
            serial_printf("Sending %d bytes via Bluetooth\n", length);
            return length; // Mock successful send
        }
    }
    
    return -1;
}

bool BluetoothDriver::isPaired(const uint8_t* address) {
    for (int i = 0; i < device_count; i++) {
        bool match = true;
        for (int j = 0; j < 6; j++) {
            if (connected_devices[i].address[j] != address[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool BluetoothDriver::pairDevice(const uint8_t* address, const char* pin) {
    if (!bluetooth_enabled) return false;
    
    serial_printf("Pairing Bluetooth device with PIN: %s\n", pin);
    
    // Mock successful pairing
    return connectToDevice(address);
}
