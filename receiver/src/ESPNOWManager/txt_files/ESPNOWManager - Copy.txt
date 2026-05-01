#include "ESPNOWManager.h"

ESPNOWCallback ESPNOWManager::user_callback_ = nullptr;

ESPNOWManager::ESPNOWManager() {
}

bool ESPNOWManager::begin() {
  Serial.println("Initializing ESPNOW...");
  
  WiFi.mode(WIFI_AP_STA); // Need both AP for web and STA for ESPNOW
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESPNOW");
    return false;
  }
  
  esp_now_register_recv_cb(ESPNOWManager::onDataReceived);
  Serial.println("ESPNOW initialized successfully");
  return true;
}

bool ESPNOWManager::addPeer(uint8_t* mac_address) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac_address, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add ESPNOW peer");
    return false;
  }
  
  Serial.print("Added ESPNOW peer: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(mac_address[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  return true;
}

void ESPNOWManager::setCallback(ESPNOWCallback callback) {
  user_callback_ = callback;
}

void ESPNOWManager::onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
  if (user_callback_ != nullptr) {
    String jsonString = String((char*)data).substring(0, len);
    user_callback_(jsonString);
  }
}