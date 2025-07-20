
#include "network.hpp"
#include "../debug/serial.hpp"

// Static member definitions
bool NetworkDriver::wifi_connected = false;
char NetworkDriver::ip_address[16] = "192.168.1.100";
char NetworkDriver::ssid[32] = "";

// Simple string copy function
static void safe_strcpy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void NetworkDriver::init() {
    serial_printf("Initializing network driver...\n");
    wifi_connected = false;
    ip_address[0] = '\0';
    ssid[0] = '\0';
    serial_printf("Network driver initialized\n");
}

bool NetworkDriver::isConnected() {
    return wifi_connected;
}

bool NetworkDriver::connectToWifi(const char* network_ssid, const char* password) {
    serial_printf("Connecting to WiFi: %s\n", network_ssid);
    
    // Simulate connection process
    safe_strcpy(ssid, network_ssid, 32);
    
    // Mock successful connection
    wifi_connected = true;
    safe_strcpy(ip_address, "192.168.1.100", 16);
    
    serial_printf("Connected to %s with IP %s\n", ssid, ip_address);
    return true;
}

void NetworkDriver::disconnect() {
    if (wifi_connected) {
        serial_printf("Disconnecting from %s\n", ssid);
        wifi_connected = false;
        ssid[0] = '\0';
        ip_address[0] = '\0';
    }
}

int NetworkDriver::sendData(const uint8_t* data, int length) {
    if (!wifi_connected) return -1;
    
    serial_printf("Sending %d bytes\n", length);
    // Mock successful send
    return length;
}

int NetworkDriver::receiveData(uint8_t* buffer, int max_length) {
    if (!wifi_connected) return -1;
    
    // Mock receiving some data
    const char* mock_data = "HTTP/1.1 200 OK\r\n\r\nHello World";
    int data_len = 0;
    while (mock_data[data_len] && data_len < max_length - 1) {
        buffer[data_len] = mock_data[data_len];
        data_len++;
    }
    buffer[data_len] = '\0';
    
    return data_len;
}

const char* NetworkDriver::getIPAddress() {
    return wifi_connected ? ip_address : "0.0.0.0";
}

void NetworkDriver::setDHCP(bool enabled) {
    serial_printf("DHCP %s\n", enabled ? "enabled" : "disabled");
}
