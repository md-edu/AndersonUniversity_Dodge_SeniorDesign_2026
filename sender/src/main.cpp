#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// ===== MODBUS PINS =====
#define RX_PIN 18
#define TX_PIN 17
#define BAUD_RATE 19200

// ===== MODBUS SETTINGS =====
#define REG_PDI_HIGH 0x03E9  // Register 41002
#define REG_PDI_LOW  0x03EA  // Register 41003
#define MODBUS_SLAVE_ID 0x01

// ===== TIMING =====
#define SEND_INTERVAL_MS 50  // 20 Hz

// ===== NODE ID =====
// This sensor maps to nodes[0] in the dashboard (Sensor 1)
// Change to 1, 2, or 3 for other corners (Sensor 2, 3, 4)
#define NODE_ID 0

// ===== RECEIVER MAC ADDRESS =====
// *** REPLACE WITH YOUR RECEIVER'S ACTUAL MAC ADDRESS ***
uint8_t receiverMAC[] = {0xDC, 0xB4, 0xD9, 0x0C, 0xD5, 0x64};

// ===== DATA STRUCTURE (must match receiver) =====
typedef struct __attribute__((packed)) {
    uint8_t  nodeId;        // 0-3 maps to Sensor 1-4
    float    distance_mm;   // Distance in millimeters
    uint32_t timestamp;     // millis() on sender
} sensor_packet_t;

// ===== UART FOR MODBUS =====
HardwareSerial ModbusSerial(1);

// ===== ESP-NOW STATUS =====
volatile bool lastSendOk = false;

// ===== CRC16 FOR MODBUS RTU =====
uint16_t crc16(uint8_t *buffer, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= buffer[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// ===== READ SINGLE MODBUS REGISTER =====
int32_t readRegister(uint16_t regAddress) {
    uint8_t request[8];
    request[0] = MODBUS_SLAVE_ID;
    request[1] = 0x03;
    request[2] = (regAddress >> 8) & 0xFF;
    request[3] = regAddress & 0xFF;
    request[4] = 0x00;
    request[5] = 0x01;
    uint16_t crc = crc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = crc >> 8;

    while (ModbusSerial.available()) ModbusSerial.read();

    ModbusSerial.write(request, 8);
    ModbusSerial.flush();

    uint8_t response[32];
    int bytesReceived = 0;
    unsigned long startTime = millis();
    unsigned long lastByteTime = 0;

    while ((millis() - startTime) < 200) {
        if (ModbusSerial.available()) {
            response[bytesReceived] = ModbusSerial.read();
            lastByteTime = millis();
            bytesReceived++;
            if (bytesReceived >= 32) break;
        }
        if (lastByteTime > 0 && (millis() - lastByteTime) > 30) break;
    }

    // Skip TX echo if present
    int start = 0;
    if (bytesReceived >= 8) {
        bool isEcho = true;
        for (int i = 0; i < 8; i++) {
            if (response[i] != request[i]) { isEcho = false; break; }
        }
        if (isEcho) start = 8;
    }

    int len = bytesReceived - start;
    uint8_t *resp = &response[start];

    if (len >= 7 && resp[0] == MODBUS_SLAVE_ID && resp[1] == 0x03 && resp[2] == 0x02) {
        uint16_t recvCrc = (resp[6] << 8) | resp[5];
        uint16_t calcCrc = crc16(resp, 5);
        if (recvCrc == calcCrc) {
            return (resp[3] << 8) | resp[4];
        }
        return -2;
    }

    if (len >= 5 && resp[0] == MODBUS_SLAVE_ID && resp[1] == 0x83) {
        return -1000 - resp[2];
    }

    return -1;
}

// ===== READ DISTANCE =====
float readDistance() {
    int32_t regHigh = readRegister(REG_PDI_HIGH);
    delay(15);
    int32_t regLow = readRegister(REG_PDI_LOW);

    if (regHigh >= 0 && regLow >= 0) {
        uint32_t combined = ((uint32_t)(uint16_t)regHigh << 16) | (uint16_t)regLow;
        return combined / 8000.0;
    }
    return -1.0;
}

// ===== ESP-NOW CALLBACK =====
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
}

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n====================================================");
    Serial.printf("   SENSOR NODE %d (Dashboard: Sensor %d)\n", NODE_ID, NODE_ID + 1);
    Serial.println("   Modbus RTU -> ESP-NOW Sender");
    Serial.println("====================================================\n");

    // Modbus UART
    pinMode(RX_PIN, INPUT_PULLUP);
    ModbusSerial.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
    delay(100);
    while (ModbusSerial.available()) ModbusSerial.read();
    Serial.println("Modbus initialized (RX:18, TX:17, 19200 baud)");

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    Serial.printf("Sender MAC: %s\n", WiFi.macAddress().c_str());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: ESP-NOW init failed!");
        return;
    }
    esp_now_register_send_cb(onDataSent);

    // Add receiver peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("ERROR: Failed to add receiver peer!");
        return;
    }
    Serial.printf("Receiver peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 receiverMAC[0], receiverMAC[1], receiverMAC[2],
                 receiverMAC[3], receiverMAC[4], receiverMAC[5]);

    // Test sensor
    Serial.println("\nTesting sensor...");
    float test = readDistance();
    if (test >= 0) {
        Serial.printf("Sensor OK! Distance: %.2f mm\n", test);
    } else {
        Serial.println("WARNING: Could not read sensor. Check wiring.");
    }

    Serial.println("\nSystem ready. Transmitting...\n");
}

// ===== MAIN LOOP =====
void loop() {
    static uint32_t lastSend = 0;
    static uint32_t txCount = 0;
    static uint32_t errCount = 0;

    if (millis() - lastSend >= SEND_INTERVAL_MS) {
        lastSend = millis();

        float distance = readDistance();

        if (distance >= 0) {
            // Build packet
            sensor_packet_t packet;
            packet.nodeId = NODE_ID;
            packet.distance_mm = distance;
            packet.timestamp = millis();

            // Send via ESP-NOW
            esp_err_t result = esp_now_send(receiverMAC,
                                            (uint8_t*)&packet,
                                            sizeof(sensor_packet_t));

            if (result == ESP_OK) {
                txCount++;
                if (txCount % 40 == 0 || txCount <= 3) {
                    Serial.printf("TX #%lu: %.2f mm\n", txCount, distance);
                }
            }
        } else {
            errCount++;
            if (errCount <= 3 || errCount % 100 == 0) {
                Serial.printf("Sensor error #%lu\n", errCount);
            }
        }
    }
}
