#include "WiFiManager.h"

WiFiManager::WiFiManager(const char* ssid, const char* password) 
  : ssid_(ssid), password_(password) {
}

bool WiFiManager::begin() {
  Serial.println("Initializing WiFi Access Point...");
  
  if (!WiFi.softAP(ssid_, password_)) {
    Serial.println("Failed to start WiFi AP");
    return false;
  }
  
  Serial.print("AP Started: ");
  Serial.println(ssid_);
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
  
  return true;
}

String WiFiManager::getIPAddress() const {
  return WiFi.softAPIP().toString();
}

String WiFiManager::getMACAddress() const {
  return WiFi.macAddress();
}