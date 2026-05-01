#ifndef WEB_SERVER_MANAGER_H
#define WEB_SERVER_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <functional>

typedef std::function<void(const String& message)> DataCallback;

class WebServerManager {
public:
  WebServerManager();
  
  bool begin();
  void handleClient();
  void setDataCallback(DataCallback callback);
  void broadcastData(const String& data);
  
private:
  AsyncWebServer server;
  DNSServer dnsServer;
  DataCallback data_callback_;
  String lastData;
  
  void setupRoutes();
  void serveFile(const String& path, const String& contentType);
  void handleSSE();
};

#endif