
#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <stdint.h>

class NetworkDriver {
public:
    static void init();
    static bool isConnected();
    static bool connectToWifi(const char* ssid, const char* password);
    static void disconnect();
    static int sendData(const uint8_t* data, int length);
    static int receiveData(uint8_t* buffer, int max_length);
    static const char* getIPAddress();
    static void setDHCP(bool enabled);
    
private:
    static bool wifi_connected;
    static char ip_address[16];
    static char ssid[32];
};

#endif
