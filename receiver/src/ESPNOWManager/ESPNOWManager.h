#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <esp_now.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <functional>

// Callback type for received data
typedef std::function<void(const String& jsonData)> ESPNOWCallback;

class ESPNOWManager {
public:
  ESPNOWManager();
  
  bool begin();
  bool addPeer(uint8_t* mac_address);
  void setCallback(ESPNOWCallback callback);
  
private:
  static void onDataReceived(const uint8_t* mac, const uint8_t* data, int len);
  static ESPNOWCallback user_callback_;
};

#endif