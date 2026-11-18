#pragma once
#include "Arduino.h"
enum wl_status_t { WL_CONNECTED = 3, WL_IDLE_STATUS = 0 };
class WiFiClass {
public:
    void begin(const char*, const char*) {}
    int status() { return WL_CONNECTED; }
    IPAddress localIP() { return IPAddress(192,168,1,50); }
};
extern WiFiClass WiFi;
