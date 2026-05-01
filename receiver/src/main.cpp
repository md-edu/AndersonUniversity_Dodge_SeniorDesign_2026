#include <Arduino.h>
#include "WiFiManager/WiFiManager.h"
#include "WebServerManager/WebServerManager.h"
#include "ESPNOWManager/ESPNOWManager.h"

// Configuration
const char* WIFI_SSID = "SensorHub-Center";
const char* WIFI_PASSWORD = "sensor123";

// ESPNOW peer MAC addresses (your C3 sender)
uint8_t senderMac[] = {0x84, 0xFC, 0xE6, 0x00, 0xFD, 0x74};


// Global Managers
WiFiManager wifi_manager(WIFI_SSID, WIFI_PASSWORD);
WebServerManager web_server_manager;
ESPNOWManager espnow_manager;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println();
  Serial.println("ESP32-S3 Central Receiver - HTTP Streaming Version");
  Serial.println("===================================================");
  
  // Initialize WiFi
  if (!wifi_manager.begin()) {
    Serial.println("WiFi setup failed! Stopping.");
    return;
  }
  
  // Print MAC address for ESPNOW configuration
  Serial.print("S3 MAC Address: ");
  Serial.println(wifi_manager.getMACAddress());
  
  // Initialize Web Server
  if (!web_server_manager.begin()) {
    Serial.println("Web server setup failed! Stopping.");
    return;
  }
  
  // Initialize ESPNOW
  if (!espnow_manager.begin()) {
    Serial.println("ESPNOW setup failed! Stopping.");
    return;
  }
  
  // Add ESPNOW peer
  espnow_manager.addPeer(senderMac);
  
  // Set ESPNOW callback
  espnow_manager.setCallback([](const String& jsonData) {
    Serial.print("ESPNOW Data Received: ");
    Serial.println(jsonData);
    
    // Store data for HTTP clients
    web_server_manager.broadcastData(jsonData);
  });
  
  // Set Web callback for any client messages
  web_server_manager.setDataCallback([](const String& message) {
    Serial.print("HTTP message from client: ");
    Serial.println(message);
  });
  
  Serial.println("System initialized successfully!");
  Serial.println("Connect to WiFi: " + String(WIFI_SSID));
  Serial.println("WebApp will open automatically");
  Serial.println("ESPNOW receiver is active");
  Serial.println("Data available via HTTP polling at /api/data");
  Serial.println("===================================================");
}

void loop() {
  web_server_manager.handleClient();
  delay(10);
}