#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>

class WiFiManager {
public:
  WiFiManager(const char* ssid, const char* password);
  
  bool begin();
  String getIPAddress() const;
  String getMACAddress() const;
  
private:
  const char* ssid_;
  const char* password_;
};

#endif