/*
 * ============================================================================
 * CENTRAL RECEIVER FIRMWARE — CNC Machine Leveling System
 * ============================================================================
 *
 * Purpose:
 *   This firmware runs on the central ESP32-S3 receiver. It performs four
 *   simultaneous roles:
 *     1. Receives wireless sensor data from up to 4 sender nodes via ESP-NOW
 *     2. Hosts a WiFi access point ("SensorHub-Center") for client devices
 *     3. Runs a captive portal so the dashboard auto-opens on connection
 *     4. Serves a real-time web dashboard from LittleFS and exposes REST APIs
 *
 * Hardware:
 *   - 1× ESP32-S3 development board
 *   - 1× 5V power source (USB or battery)
 *   - No additional wiring required — all communication is wireless
 *
 * Data Flow:
 *   Sender Nodes → (ESP-NOW) → This Receiver → (WiFi AP) → Browser Dashboard
 *                                    │
 *                              LittleFS stores
 *                              HTML/CSS/JS/images
 *
 * API Endpoints:
 *   GET /api/data   — Returns JSON with all 4 sensor node states
 *   GET /api/status — Returns JSON with system info (MAC, IP, uptime, etc.)
 *
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <LittleFS.h>

// ===== WIFI ACCESS POINT SETTINGS =====
// These credentials are used by client devices (phones, laptops, tablets)
// to connect to the receiver's WiFi network and view the dashboard.
const char* AP_SSID = "SensorHub-Center";
const char* AP_PASS = "sensor123";

// ===== SENDER MAC ADDRESS =====
// The MAC address of the sender node that this receiver will accept ESP-NOW
// packets from. This must match the sender board's actual WiFi MAC address.
// To obtain this address, see the "Obtaining ESP32-S3 MAC Addresses" section
// in the README.
// If using multiple sender nodes, additional peers must be registered in setup().
uint8_t senderMAC[] = {0x1C, 0xDB, 0xD4, 0x9C, 0x23, 0x14};

// ===== ESP-NOW DATA PACKET STRUCTURE =====
// This struct defines the exact byte layout of each wireless transmission.
// It MUST be identical to the struct defined in the sender firmware.
// The __attribute__((packed)) directive prevents compiler padding, ensuring
// the struct is exactly 9 bytes and matches the sender byte-for-byte.
typedef struct __attribute__((packed)) {
    uint8_t  nodeId;        // 1 byte:  Identifies which corner (0-3)
    float    distance_mm;   // 4 bytes: Distance measurement in millimeters
    uint32_t timestamp;     // 4 bytes: Sender's millis() at time of reading
} sensor_packet_t;          // Total: 9 bytes per transmission

// ===== NODE STATE TRACKING =====
// The receiver tracks the state of up to 4 sensor nodes (one per corner of
// the CNC machine bed). Each node can be online or offline.
#define NUM_NODES 4
#define NODE_TIMEOUT_MS 3000  // If no packet received for 3 seconds, mark offline

// Stores the latest data and connection status for each sensor node.
struct NodeState {
    bool     online;        // true if node has sent data recently
    float    distance_mm;   // Most recent distance reading from this node
    uint32_t lastSeen;      // millis() timestamp when last packet was received
};

// Array of node states, indexed by nodeId (0-3).
// nodes[0] = Sensor 1 (front-left), nodes[1] = Sensor 2 (front-right), etc.
NodeState nodes[NUM_NODES];

// ===== GLOBAL OBJECTS =====
// WebServer handles HTTP requests from connected clients (browsers).
// DNSServer intercepts all DNS queries to redirect them to the captive portal.
WebServer server(80);       // HTTP server on port 80 (standard web port)
DNSServer dnsServer;        // DNS server for captive portal redirection

// Counter for total ESP-NOW packets received since boot (used for diagnostics).
uint32_t packetsReceived = 0;

// =============================================================================
// MIME TYPE HELPER
// =============================================================================
// Returns the correct MIME type string for a given filename based on its
// extension. The browser uses this to know how to interpret each file
// (e.g., render HTML, apply CSS, execute JavaScript, display images).
//
// Parameters:
//   filename — the name or path of the file being served
//
// Returns: MIME type string (e.g., "text/html", "image/png")
// =============================================================================
String getMimeType(String filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css"))  return "text/css";
    if (filename.endsWith(".js"))   return "application/javascript";
    if (filename.endsWith(".png"))  return "image/png";
    if (filename.endsWith(".jpg"))  return "image/jpeg";
    if (filename.endsWith(".ico"))  return "image/x-icon";
    if (filename.endsWith(".svg"))  return "image/svg+xml";
    return "text/plain";  // Default fallback for unknown file types
}

// =============================================================================
// SERVE FILE FROM LITTLEFS
// =============================================================================
// Attempts to open and stream a file from the ESP32-S3's LittleFS flash
// filesystem to the requesting HTTP client. LittleFS stores the web dashboard
// files (HTML, CSS, JS, images) that were uploaded via `pio run --target uploadfs`.
//
// If the requested path ends with "/", it automatically appends "index.html"
// to serve the default page (standard web server behavior).
//
// Parameters:
//   path — the URI path requested by the client (e.g., "/style.css")
//
// Returns:
//   true  — file was found and served successfully
//   false — file does not exist in LittleFS
// =============================================================================
bool serveFile(String path) {
    // If the path is a directory (ends with /), serve the default index page
    if (path.endsWith("/")) path += "index.html";

    // Check if the file exists in the LittleFS filesystem
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");               // Open file for reading
        server.streamFile(file, getMimeType(path));          // Stream to client with correct MIME type
        file.close();                                        // Release the file handle
        return true;
    }
    return false;  // File not found
}

// =============================================================================
// ESP-NOW RECEIVE CALLBACK
// =============================================================================
// Called automatically by the ESP-NOW stack whenever a packet arrives from
// any registered sender peer. This function runs in WiFi task context
// (not the main loop), so it should be kept short and avoid blocking operations.
//
// The function validates the incoming packet by checking its size matches
// the expected sensor_packet_t struct, then extracts the data and updates
// the corresponding node's state in the nodes[] array.
//
// Parameters:
//   mac  — MAC address of the sender (6 bytes)
//   data — pointer to the raw received data bytes
//   len  — number of bytes received
// =============================================================================
void onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
    // Validate packet size — must exactly match our struct to avoid memory issues
    if (len == sizeof(sensor_packet_t)) {
        // Copy raw bytes into a typed struct for safe field access.
        // Using memcpy instead of a direct cast avoids alignment issues on ESP32.
        sensor_packet_t packet;
        memcpy(&packet, data, sizeof(sensor_packet_t));

        // Validate nodeId is within bounds (0-3) to prevent array overflow
        if (packet.nodeId < NUM_NODES) {
            // Update the node's state with the new reading
            nodes[packet.nodeId].online = true;
            nodes[packet.nodeId].distance_mm = packet.distance_mm;
            nodes[packet.nodeId].lastSeen = millis();

            // Increment global packet counter for diagnostics
            packetsReceived++;

            // Print periodic status to serial monitor for debugging.
            // Prints the first 5 packets (to confirm startup), then every 40th
            // packet to avoid flooding the serial output at 20 Hz per node.
            if (packetsReceived % 40 == 0 || packetsReceived <= 5) {
                Serial.printf("RX #%lu: Node %d = %.2f mm\n",
                             packetsReceived, packet.nodeId, packet.distance_mm);
            }
        }
    }
}

// =============================================================================
// API ENDPOINT: GET /api/data
// =============================================================================
// Returns a JSON object containing the current state of all 4 sensor nodes.
// The web dashboard's script.js polls this endpoint every second to update
// the display.
//
// Before building the response, this function checks each node for timeout:
// if no packet has been received from a node in the last NODE_TIMEOUT_MS
// milliseconds (3 seconds), that node is marked offline.
//
// Response JSON format:
// {
//   "timestamp": 123456,          // Receiver's millis() at response time
//   "uptime": 123456,             // Same as timestamp (millis since boot)
//   "packets": 5000,              // Total ESP-NOW packets received
//   "nodes": [
//     {"id": 0, "online": true,  "distance": 123.45},
//     {"id": 1, "online": false, "distance": 0.00},
//     {"id": 2, "online": false, "distance": 0.00},
//     {"id": 3, "online": false, "distance": 0.00}
//   ]
// }
// =============================================================================
void handleApiData() {
    // --- Check for node timeouts ---
    // A node is marked offline if it hasn't sent data within NODE_TIMEOUT_MS.
    // This handles cases where a sender loses power or goes out of range.
    for (int i = 0; i < NUM_NODES; i++) {
        if (nodes[i].online && (millis() - nodes[i].lastSeen > NODE_TIMEOUT_MS)) {
            nodes[i].online = false;
        }
    }

    // --- Build JSON response string ---
    // Manually constructed (no ArduinoJson library needed for this simple format).
    // The format matches what script.js expects in its updateDisplay() function.
    String json = "{";
    json += "\"timestamp\":" + String(millis()) + ",";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"packets\":" + String(packetsReceived) + ",";
    json += "\"nodes\":[";

    for (int i = 0; i < NUM_NODES; i++) {
        if (i > 0) json += ",";  // Comma separator between node objects
        json += "{";
        json += "\"id\":" + String(i) + ",";
        json += "\"online\":" + String(nodes[i].online ? "true" : "false") + ",";
        json += "\"distance\":" + String(nodes[i].distance_mm, 2);  // 2 decimal places
        json += "}";
    }

    json += "]}";

    // Send the JSON response with HTTP 200 OK and correct content type
    server.send(200, "application/json", json);
}

// =============================================================================
// API ENDPOINT: GET /api/status
// =============================================================================
// Returns a JSON object containing system-level information about the receiver.
// Useful for diagnostics and monitoring the health of the central hub.
//
// Response JSON format:
// {
//   "ssid": "SensorHub-Center",
//   "ip": "192.168.4.1",
//   "mac": "DC:B4:D9:0C:D5:64",
//   "uptime": 123456,
//   "packets": 5000,
//   "clients": 2
// }
// =============================================================================
void handleApiStatus() {
    String json = "{";
    json += "\"ssid\":\"" + String(AP_SSID) + "\",";                    // WiFi network name
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";           // AP IP address (usually 192.168.4.1)
    json += "\"mac\":\"" + WiFi.macAddress() + "\",";                   // Receiver's WiFi MAC address
    json += "\"uptime\":" + String(millis()) + ",";                     // Milliseconds since boot
    json += "\"packets\":" + String(packetsReceived) + ",";             // Total ESP-NOW packets received
    json += "\"clients\":" + String(WiFi.softAPgetStationNum());        // Number of WiFi clients connected
    json += "}";

    server.send(200, "application/json", json);
}

// =============================================================================
// CAPTIVE PORTAL CATCH-ALL HANDLER
// =============================================================================
// This is the "not found" handler — it catches any HTTP request that doesn't
// match a registered route. It first tries to serve the requested file from
// LittleFS (in case it's a static asset we didn't explicitly register).
// If the file doesn't exist, it serves index.html instead.
//
// This behavior is essential for the captive portal: when a device connects
// to the WiFi AP, it probes various URLs to detect internet connectivity.
// By responding to ALL requests with the dashboard page, the device's OS
// recognizes this as a captive portal and opens the dashboard automatically.
// =============================================================================
void handleCaptivePortal() {
    if (!serveFile(server.uri())) {
        // File not found in LittleFS — serve the main dashboard page instead.
        // This ensures any unknown URL redirects to the dashboard.
        serveFile("/index.html");
    }
}

// =============================================================================
// SETUP — Runs once at power-on or reset
// =============================================================================
void setup() {
    // Initialize USB serial for debug output (visible in PlatformIO serial monitor)
    Serial.begin(115200);
    delay(1000);  // Allow time for serial monitor to connect

    // Print startup banner for identification
    Serial.println();
    Serial.println("====================================================");
    Serial.println("   ESP32-S3 Central Receiver");
    Serial.println("   ESP-NOW -> Web Dashboard");
    Serial.println("====================================================");

    // --- Initialize node state array ---
    // All nodes start as offline with zero distance until data arrives.
    for (int i = 0; i < NUM_NODES; i++) {
        nodes[i].online = false;
        nodes[i].distance_mm = 0.0;
        nodes[i].lastSeen = 0;
    }

    // =====================================================================
    // LITTLEFS FILESYSTEM INITIALIZATION
    // =====================================================================
    // LittleFS is a lightweight filesystem for microcontrollers that stores
    // files in the ESP32-S3's flash memory. The web dashboard files (HTML,
    // CSS, JS, images) are uploaded to this filesystem using:
    //   pio run --target uploadfs
    //
    // The `true` parameter in LittleFS.begin(true) enables auto-formatting
    // of the partition if it's not already formatted (first boot after flash).
    // =====================================================================
    Serial.println("\nMounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("ERROR: LittleFS mount failed!");
        return;  // Cannot serve dashboard without filesystem
    }

    // List all files stored in LittleFS for verification.
    // This helps confirm that the uploadfs step was successful.
    // Expected files: index.html, style.css, script.js, logo.png, building.png
    Serial.println("Files in LittleFS:");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }

    // =====================================================================
    // WIFI ACCESS POINT INITIALIZATION
    // =====================================================================
    // The ESP32-S3 is set to WIFI_AP_STA mode, which runs both an Access
    // Point (AP) and a Station (STA) interface simultaneously. This is
    // required because:
    //   - AP mode: Hosts the WiFi network for client devices (browsers)
    //   - STA mode: Required for ESP-NOW to function alongside the AP
    //
    // ESP-NOW uses the WiFi radio at the physical layer but does not require
    // an active WiFi connection. Running in AP_STA mode allows both the
    // WiFi AP and ESP-NOW to coexist on the same radio.
    // =====================================================================
    Serial.println("\nStarting WiFi Access Point...");
    WiFi.mode(WIFI_AP_STA);

    if (!WiFi.softAP(AP_SSID, AP_PASS)) {
        Serial.println("ERROR: Failed to start WiFi AP!");
        return;  // Cannot serve dashboard without WiFi
    }
    delay(100);  // Allow AP to stabilize before reading IP

    // Print AP configuration for reference
    Serial.printf("AP SSID: %s\n", AP_SSID);
    Serial.printf("AP Password: %s\n", AP_PASS);
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());

    // =====================================================================
    // DNS SERVER — CAPTIVE PORTAL REDIRECT
    // =====================================================================
    // The DNS server listens on port 53 (standard DNS port) and responds to
    // ALL domain name queries with the ESP32-S3's AP IP address (192.168.4.1).
    // The wildcard "*" means every domain resolves to our device.
    //
    // This is the core mechanism of the captive portal: when a client device
    // tries to resolve any domain (e.g., google.com), it gets 192.168.4.1,
    // which serves our dashboard. The device's OS detects this as a captive
    // portal and prompts the user to "sign in" — which opens our dashboard.
    // =====================================================================
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.println("DNS captive portal started");

    // =====================================================================
    // WEB SERVER ROUTE REGISTRATION
    // =====================================================================
    // Routes are registered in order of specificity. The web server matches
    // incoming HTTP requests against these routes and calls the corresponding
    // handler function.
    // =====================================================================

    // --- REST API endpoints ---
    // These return JSON data consumed by the dashboard's JavaScript.
    server.on("/api/data", HTTP_GET, handleApiData);       // Sensor data for dashboard
    server.on("/api/status", HTTP_GET, handleApiStatus);   // System diagnostics

    // --- Static file routes ---
    // Explicitly register each dashboard file. When a browser loads index.html,
    // it requests style.css, script.js, and the image files. Each request is
    // handled by serving the corresponding file from LittleFS.
    server.on("/", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/index.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/style.css", HTTP_GET, []() { serveFile("/style.css"); });
    server.on("/script.js", HTTP_GET, []() { serveFile("/script.js"); });
    server.on("/logo.png", HTTP_GET, []() { serveFile("/logo.png"); });
    server.on("/building.png", HTTP_GET, []() { serveFile("/building.png"); });

    // --- Captive portal detection URLs ---
    // Different operating systems probe specific URLs to detect captive portals:
    //   Android:  /generate_204, /gen_204
    //   iOS:      /hotspot-detect.html, /canonical.html
    //   Windows:  /connecttest.txt, /fwlink, /redirect
    //   macOS:    /success.txt
    //
    // By serving our dashboard page for all of these, the OS recognizes a
    // captive portal and automatically opens the dashboard in a popup browser.
    server.on("/generate_204", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/gen_204", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/hotspot-detect.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/canonical.html", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/success.txt", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/fwlink", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/connecttest.txt", HTTP_GET, []() { serveFile("/index.html"); });
    server.on("/redirect", HTTP_GET, []() { serveFile("/index.html"); });

    // --- Catch-all handler ---
    // Any request that doesn't match the routes above falls through to this
    // handler, which tries to serve the file from LittleFS or defaults to
    // the dashboard page. This ensures the captive portal works even for
    // unexpected probe URLs from different OS versions.
    server.onNotFound(handleCaptivePortal);

    // Start the web server — it will now respond to HTTP requests on port 80
    server.begin();
    Serial.println("Web server started on port 80");

    // =====================================================================
    // ESP-NOW INITIALIZATION
    // =====================================================================
    // ESP-NOW is a connectionless wireless protocol by Espressif that allows
    // ESP32 devices to communicate directly without a WiFi router. It operates
    // at the MAC layer with very low latency (~1-2ms) and supports up to
    // 250 bytes per packet.
    //
    // The receiver registers a callback function that fires every time a
    // packet arrives, then adds the sender as a known peer.
    // =====================================================================
    Serial.println("\nInitializing ESP-NOW...");
    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: ESP-NOW init failed!");
        return;  // Cannot receive sensor data without ESP-NOW
    }

    // Register the receive callback — onDataReceived() will be called
    // automatically whenever an ESP-NOW packet arrives from any peer.
    esp_now_register_recv_cb(onDataReceived);

    // --- Register the sender as an ESP-NOW peer ---
    // ESP-NOW requires explicitly adding each device you want to communicate with.
    // Channel 1 matches the WiFi AP channel (the AP defaults to channel 1).
    // Encryption is disabled for simplicity and lower latency.
    //
    // NOTE: To support multiple sender nodes, duplicate this block for each
    // sender with its unique MAC address. The onDataReceived callback already
    // handles multiple nodes via the nodeId field in the packet.
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, senderMAC, 6);
    peerInfo.channel = 1;       // Match the WiFi AP channel
    peerInfo.encrypt = false;   // No encryption

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("ERROR: Failed to add sender peer!");
    } else {
        Serial.printf("Sender peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                     senderMAC[0], senderMAC[1], senderMAC[2],
                     senderMAC[3], senderMAC[4], senderMAC[5]);
    }

    // --- System ready ---
    Serial.println("\n====================================================");
    Serial.println("System ready!");
    Serial.printf("Connect to WiFi: %s (password: %s)\n", AP_SSID, AP_PASS);
    Serial.println("Dashboard: http://192.168.4.1");
    Serial.println("====================================================\n");
}

// =============================================================================
// MAIN LOOP — Runs continuously after setup()
// =============================================================================
// The main loop has two responsibilities:
//   1. Process DNS and HTTP requests (keeps the captive portal and web server alive)
//   2. Print periodic status updates to the serial monitor for diagnostics
//
// Sensor data reception is handled asynchronously by the onDataReceived()
// callback — it does NOT happen in this loop. The ESP-NOW stack calls
// onDataReceived() in the WiFi task whenever a packet arrives.
// =============================================================================
void loop() {
    // --- Process pending DNS requests ---
    // The DNS server must be polled regularly to respond to domain name queries.
    // Without this, the captive portal redirect stops working.
    dnsServer.processNextRequest();

    // --- Process pending HTTP requests ---
    // The web server must be polled regularly to handle incoming HTTP requests
    // from connected browsers. Without this, the dashboard stops responding.
    server.handleClient();

    // --- Periodic status report (every 30 seconds) ---
    // Prints a summary to the serial monitor showing packet count, connected
    // WiFi clients, uptime, and the status of each online sensor node.
    // Useful for monitoring the system without a browser.
    static uint32_t lastStatus = 0;  // Persists across loop iterations
    if (millis() - lastStatus >= 30000) {
        lastStatus = millis();

        // Print overall system status
        Serial.printf("[Status] Packets: %lu | WiFi Clients: %d | Uptime: %lu s\n",
                     packetsReceived,
                     WiFi.softAPgetStationNum(),
                     millis() / 1000);

        // Print status of each online node
        for (int i = 0; i < NUM_NODES; i++) {
            if (nodes[i].online) {
                Serial.printf("  Node %d: %.2f mm (last seen %lu ms ago)\n",
                             i, nodes[i].distance_mm, millis() - nodes[i].lastSeen);
            }
        }
    }
}