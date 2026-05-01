#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <LittleFS.h>

// ===== WIFI AP SETTINGS =====
const char* AP_SSID = "SensorHub-Center";
const char* AP_PASS = "sensor123";

// ===== SENDER MAC ADDRESS =====
uint8_t senderMAC[] = {0x1C, 0xDB, 0xD4, 0x9C, 0x23, 0x14};

// ===== DATA STRUCTURE (must match sender) =====
typedef struct __attribute__((packed)) {
    uint8_t  nodeId;        // 0-3 maps to Sensor 1-4
    float    distance_mm;   // Distance in millimeters
    uint32_t timestamp;     // millis() on sender
} sensor_packet_t;

// ===== NODE STATE =====
#define NUM_NODES 4
#define NODE_TIMEOUT_MS 3000  // Mark offline after 3 seconds of no data

struct NodeState {
    bool    online;
    float   distance_mm;
    uint32_t lastSeen;      // millis() when last packet received
};

NodeState nodes[NUM_NODES];

// ===== GLOBALS =====
WebServer server(80);
DNSServer dnsServer;
uint32_t packetsReceived = 0;

// ===== MIME TYPE HELPER =====
String getMimeType(String filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css"))  return "text/css";
    if (filename.endsWith(".js"))   return "application/javascript";
    if (filename.endsWith(".png"))  return "image/png";
    if (filename.endsWith(".jpg"))  return "image/jpeg";
    if (filename.endsWith(".ico"))  return "image/x-icon";
    if (filename.endsWith(".svg"))  return "image/svg+xml";
    return "text/plain";
}

// ===== SERVE FILE FROM LITTLEFS =====
bool serveFile(String path) {
    if (path.endsWith("/")) path += "index.html";
    
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        server.streamFile(file, getMimeType(path));
        file.close();
        return true;
    }
    return false;
}

// ===== ESP-NOW RECEIVE CALLBACK =====
void onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(sensor_packet_t)) {
        sensor_packet_t packet;
        memcpy(&packet, data, sizeof(sensor_packet_t));

        if (packet.nodeId < NUM_NODES) {
            nodes[packet.nodeId].online = true;
            nodes[packet.nodeId].distance_mm = packet.distance_mm;
            nodes[packet.nodeId].lastSeen = millis();
            packetsReceived++;

            if (packetsReceived % 40 == 0 || packetsReceived <= 5) {
                Serial.printf("RX #%lu: Node %d = %.2f mm\n",
                             packetsReceived, packet.nodeId, packet.distance_mm);
            }
        }
    }
}

// ===== API: /api/data =====
void handleApiData() {
    // Check timeouts
    for (int i = 0; i < NUM_NODES; i++) {
        if (nodes[i].online && (millis() - nodes[i].lastSeen > NODE_TIMEOUT_MS)) {
            nodes[i].online = false;
        }
    }

    // Build JSON matching script.js expected format:
    // { "timestamp": ..., "uptime": ..., "nodes": [ {"id":0,"online":true,"distance":123.45}, ... ] }
    String json = "{";
    json += "\"timestamp\":" + String(millis()) + ",";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"packets\":" + String(packetsReceived) + ",";
    json += "\"nodes\":[";

    for (int i = 0; i < NUM_NODES; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"id\":" + String(i) + ",";
        json += "\"online\":" + String(nodes[i].online ? "true" : "false") + ",";
        json += "\"distance\":" + String(nodes[i].distance_mm, 2);
        json += "}";
    }

    json += "]}";

    server.send(200, "application/json", json);
}

// ===== API: /api/status =====
void handleApiStatus() {
    String json = "{";
    json += "\"ssid\":\"" + String(AP_SSID) + "\",";
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"mac\":\"" + WiFi.macAddress() + "\",";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"packets\":" + String(packetsReceived) + ",";
    json += "\"clients\":" + String(WiFi.softAPgetStationNum());
    json += "}";

    server.send(200, "application/json", json);
}

// ===== CAPTIVE PORTAL HANDLER =====
void handleCaptivePortal() {
    if (!serveFile(server.uri())) {
        // If file not found, serve index.html (captive portal redirect)
        serveFile("/index.html");
    }
}

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("====================================================");
    Serial.println("   ESP32-S3 Central Receiver");
    Serial.println("   ESP-NOW -> Web Dashboard");
    Serial.println("====================================================");

    // Initialize node states
    for (int i = 0; i < NUM_NODES; i++) {
        nodes[i].online = false;
        nodes[i].distance_mm = 0.0;
        nodes[i].lastSeen = 0;
    }

    // ----- LittleFS -----
    Serial.println("\nMounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("ERROR: LittleFS mount failed!");
        return;
    }

    // List files for verification
    Serial.println("Files in LittleFS:");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }

    // ----- WiFi AP -----
    Serial.println("\nStarting WiFi Access Point...");
    WiFi.mode(WIFI_AP_STA);

    if (!WiFi.softAP(AP_SSID, AP_PASS)) {
        Serial.println("ERROR: Failed to start WiFi AP!");
        return;
    }

    delay(100);
    Serial.printf("AP SSID: %s\n", AP_SSID);
    Serial.printf("AP Password: %s\n", AP_PASS);
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());

    // ----- DNS (Captive Portal) -----
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.println("DNS captive portal started");

    // ----- Web Server Routes -----
    // API endpoints
    server.on("/api/data", HTTP_GET, handleApiData);
    server.on("/api/status", HTTP_GET, handleApiStatus);

    // Static files
    server.on("/", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/index.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/style.css", HTTP_GET, []() { serveFile("/style.css"); });
    server.on("/script.js", HTTP_GET, []() { serveFile("/script.js"); });
    server.on("/logo.png", HTTP_GET, []() { serveFile("/logo.png"); });
    server.on("/building.png", HTTP_GET, []() { serveFile("/building.png"); });

    // Captive portal detection URLs
    server.on("/generate_204", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/gen_204", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/hotspot-detect.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/canonical.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/success.txt", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/fwlink", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/connecttest.txt", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/redirect", HTTP_GET, []() { serveFile("/index.html"); });

    // Catch-all for any other requests
    server.onNotFound(handleCaptivePortal);

    server.begin();
    Serial.println("Web server started on port 80");

    // ----- ESP-NOW -----
    Serial.println("\nInitializing ESP-NOW...");
    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: ESP-NOW init failed!");
        return;
    }

    esp_now_register_recv_cb(onDataReceived);

    // Add sender as peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, senderMAC, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("ERROR: Failed to add sender peer!");
    } else {
        Serial.printf("Sender peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                     senderMAC[0], senderMAC[1], senderMAC[2],
                     senderMAC[3], senderMAC[4], senderMAC[5]);
    }

    // ----- Ready -----
    Serial.println("\n====================================================");
    Serial.println("System ready!");
    Serial.printf("Connect to WiFi: %s (password: %s)\n", AP_SSID, AP_PASS);
    Serial.println("Dashboard: http://192.168.4.1");
    Serial.println("====================================================\n");
}

// ===== MAIN LOOP =====
void loop() {
    dnsServer.processNextRequest();
    server.handleClient();

    // Status print every 30 seconds
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 30000) {
        lastStatus = millis();
        Serial.printf("[Status] Packets: %lu | WiFi Clients: %d | Uptime: %lu s\n",
                     packetsReceived,
                     WiFi.softAPgetStationNum(),
                     millis() / 1000);
        for (int i = 0; i < NUM_NODES; i++) {
            if (nodes[i].online) {
                Serial.printf("  Node %d: %.2f mm (last seen %lu ms ago)\n",
                             i, nodes[i].distance_mm, millis() - nodes[i].lastSeen);
            }
        }
    }
}
