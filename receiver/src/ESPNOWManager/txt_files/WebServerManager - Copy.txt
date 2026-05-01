#include "WebServerManager.h"
#include <WiFi.h>

WebServerManager::WebServerManager() : server(80) {
}

bool WebServerManager::begin() {
  Serial.println("Initializing Web Server with HTTP Streaming...");
  
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed!");
    return false;
  }
  Serial.println("LittleFS mounted successfully");
  
  // Start DNS server for captive portal
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  setupRoutes();
  
  server.begin();
  Serial.println("Web server started on port 80");
  return true;
}

void WebServerManager::handleClient() {
  dnsServer.processNextRequest();
}

void WebServerManager::setDataCallback(DataCallback callback) {
  data_callback_ = callback;
}

void WebServerManager::broadcastData(const String& data) {
  lastData = data;
  // Data will be sent when clients request it via polling
}

void WebServerManager::setupRoutes() {
  // Serve main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  // Serve static files
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });
  
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/script.js", "application/javascript");
  });
  
  server.on("/logo.png", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/logo.png", "image/png");
  });
  
  server.on("/building.png", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/building.png", "image/png");
  });
  
  // API endpoint to get latest data (polling)
  server.on("/api/data", HTTP_GET, [this](AsyncWebServerRequest *request){
    request->send(200, "application/json", lastData);
  });
  
  // API endpoint to send test data
  server.on("/api/send", HTTP_POST, [this](AsyncWebServerRequest *request){}, NULL,
    [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = "";
      for(size_t i = 0; i < len; i++){
        body += (char)data[i];
      }
      if (data_callback_) {
        data_callback_(body);
      }
      request->send(200, "application/json", "{\"status\":\"received\"}");
    });
  
  // Captive portal detection URLs - ALL redirect to index
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/success.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  // Catch-all handler for any other request
  server.onNotFound([](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
}