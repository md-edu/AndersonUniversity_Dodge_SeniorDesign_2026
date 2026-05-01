/*
 * ============================================================================
 * SENDER NODE FIRMWARE — CNC Machine Leveling System
 * ============================================================================
 * 
 * Purpose:
 *   This firmware runs on an ESP32-S3 at each corner of the CNC machine bed.
 *   It reads distance measurements from a Banner LE250 laser displacement sensor
 *   via Modbus RTU (through a Banner R45C IO-Link master and RS-485-to-TTL
 *   converter), then wirelessly transmits the readings to a central receiver
 *   using ESP-NOW at 20 Hz.
 *
 * Hardware Connections:
 *   Banner LE250 → R45C IO-Link Master (direct M12 connection)
 *   R45C Red wire   → 24V battery (+)
 *   R45C Blue wire  → 24V battery (−) / GND
 *   R45C Black wire → RS-485-to-TTL module A+ terminal
 *   R45C White wire → RS-485-to-TTL module B− terminal
 *   RS-485 RX       → ESP32-S3 GPIO 18
 *   RS-485 TX       → ESP32-S3 GPIO 17
 *   RS-485 VCC      → ESP32-S3 3.3V
 *   RS-485 GND      → Common GND
 *   ESP32-S3        → 5V power source
 *
 * Communication Flow:
 *   Banner LE250 → (IO-Link) → R45C → (RS-485 Modbus RTU) → ESP32-S3 → (ESP-NOW) → Receiver
 *
 * ============================================================================
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// ===== MODBUS UART PIN DEFINITIONS =====
// These pins connect to the RS-485-to-TTL converter module.
// The converter translates RS-485 differential signals from the R45C
// into 3.3V TTL-level UART signals the ESP32 can read.
#define RX_PIN 18        // ESP32 receives Modbus responses on GPIO 18
#define TX_PIN 17        // ESP32 sends Modbus requests on GPIO 17
#define BAUD_RATE 19200  // R45C default Modbus baud rate

// ===== MODBUS REGISTER ADDRESSES =====
// The Banner LE250 distance measurement is stored as a 32-bit value split
// across two consecutive 16-bit Modbus holding registers on the R45C.
// These addresses were discovered by scanning ~300 registers and identifying
// which ones contained live data that changed with sensor movement.
// Register 41002 holds the high 16 bits, register 41003 holds the low 16 bits.
// Note: Modbus protocol uses 0-indexed addresses, so register 41002 = address 0x03E9.
#define REG_PDI_HIGH 0x03E9  // Register 41002 — high word of distance value
#define REG_PDI_LOW  0x03EA  // Register 41003 — low word of distance value

// ===== MODBUS SLAVE ID =====
// The R45C IO-Link master responds to Modbus slave address 1 by default.
#define MODBUS_SLAVE_ID 0x01

// ===== TIMING CONFIGURATION =====
// Controls how frequently the sensor is read and data is transmitted.
// 50ms interval = 20 readings per second (20 Hz update rate).
#define SEND_INTERVAL_MS 50

// ===== NODE IDENTIFICATION =====
// Each sensor node must have a unique ID (0-3) corresponding to its position
// on the CNC machine bed. This ID tells the receiver which corner the data
// belongs to, and maps to the dashboard display as follows:
//   NODE_ID 0 → Sensor 1 (e.g., front-left corner)
//   NODE_ID 1 → Sensor 2 (e.g., front-right corner)
//   NODE_ID 2 → Sensor 3 (e.g., rear-left corner)
//   NODE_ID 3 → Sensor 4 (e.g., rear-right corner)
// IMPORTANT: Change this value for each physical sensor node before flashing.
#define NODE_ID 0

// ===== RECEIVER MAC ADDRESS =====
// The ESP-NOW protocol requires the receiver's WiFi MAC address to send
// directed (unicast) packets. This must match the MAC address printed by
// the receiver firmware on startup.
// *** REPLACE WITH YOUR RECEIVER'S ACTUAL MAC ADDRESS ***
uint8_t receiverMAC[] = {0xDC, 0xB4, 0xD9, 0x0C, 0xD5, 0x64};

// ===== ESP-NOW DATA PACKET STRUCTURE =====
// This struct defines the exact byte layout of each wireless transmission.
// The __attribute__((packed)) directive prevents the compiler from inserting
// padding bytes, ensuring the struct is exactly 9 bytes and matches the
// receiver's definition byte-for-byte.
typedef struct __attribute__((packed)) {
    uint8_t  nodeId;        // 1 byte:  Identifies which corner (0-3)
    float    distance_mm;   // 4 bytes: Distance measurement in millimeters
    uint32_t timestamp;     // 4 bytes: Sender's millis() at time of reading
} sensor_packet_t;          // Total: 9 bytes per transmission

// ===== HARDWARE SERIAL FOR MODBUS =====
// The ESP32-S3 has multiple hardware UART peripherals. We use UART1 (index 1)
// for Modbus communication, keeping UART0 (Serial) free for debug output via USB.
HardwareSerial ModbusSerial(1);

// ===== ESP-NOW TRANSMISSION STATUS =====
// Updated by the send callback to indicate whether the last packet was
// acknowledged by the receiver. Declared volatile because it's modified
// in an interrupt context (the ESP-NOW send callback).
volatile bool lastSendOk = false;

// =============================================================================
// CRC-16 CALCULATION FOR MODBUS RTU
// =============================================================================
// Modbus RTU uses CRC-16 with polynomial 0xA001 (bit-reversed 0x8005) for
// error detection. Every Modbus frame ends with a 2-byte CRC that the receiver
// recalculates and compares to verify data integrity.
//
// Algorithm: For each byte, XOR it into the CRC register, then for each of
// the 8 bits, if the LSB is 1, shift right and XOR with 0xA001; otherwise
// just shift right.
//
// Parameters:
//   buffer — pointer to the data bytes to calculate CRC over
//   length — number of bytes in the buffer
//
// Returns: 16-bit CRC value (low byte first in Modbus frames)
// =============================================================================
uint16_t crc16(uint8_t *buffer, uint16_t length) {
    uint16_t crc = 0xFFFF;  // Initialize CRC register to all 1s (Modbus standard)
    for (uint16_t i = 0; i < length; i++) {
        crc ^= buffer[i];   // XOR current byte into low byte of CRC
        for (uint8_t j = 0; j < 8; j++) {  // Process each bit
            if (crc & 0x0001) {             // If LSB is set:
                crc >>= 1;                  //   Shift right
                crc ^= 0xA001;              //   XOR with reversed polynomial
            } else {
                crc >>= 1;                  // Otherwise just shift right
            }
        }
    }
    return crc;
}

// =============================================================================
// READ A SINGLE MODBUS HOLDING REGISTER
// =============================================================================
// Constructs and sends a Modbus RTU "Read Holding Registers" (function code 0x03)
// request for a single register, then parses the response.
//
// This function handles a quirk of the RS-485-to-TTL converter: because the
// DE (Driver Enable) and RE (Receiver Enable) pins are tied together for
// auto-direction control, the module echoes back every byte we transmit on
// the RX line. The function detects and strips this echo before parsing
// the actual response from the R45C.
//
// Parameters:
//   regAddress — the 0-indexed Modbus register address to read
//
// Returns:
//   >= 0:    The 16-bit register value (success)
//   -1:      No valid response received (timeout or malformed)
//   -2:      CRC mismatch in response (data corruption)
//   -1000-N: Modbus exception response with error code N
// =============================================================================
int32_t readRegister(uint16_t regAddress) {
    // --- Build the Modbus RTU request frame ---
    // Format: [SlaveID][FuncCode][RegAddrHi][RegAddrLo][QuantityHi][QuantityLo][CRC_Lo][CRC_Hi]
    uint8_t request[8];
    request[0] = MODBUS_SLAVE_ID;           // Slave address (0x01)
    request[1] = 0x03;                      // Function code: Read Holding Registers
    request[2] = (regAddress >> 8) & 0xFF;  // Register address high byte
    request[3] = regAddress & 0xFF;         // Register address low byte
    request[4] = 0x00;                      // Quantity of registers high byte (0)
    request[5] = 0x01;                      // Quantity of registers low byte (1 register)

    // Calculate and append CRC-16 (low byte first per Modbus RTU spec)
    uint16_t crc = crc16(request, 6);
    request[6] = crc & 0xFF;        // CRC low byte
    request[7] = crc >> 8;          // CRC high byte

    // --- Clear any stale data in the receive buffer ---
    while (ModbusSerial.available()) ModbusSerial.read();

    // --- Transmit the request ---
    ModbusSerial.write(request, 8);
    ModbusSerial.flush();  // Wait until all bytes are physically sent

    // --- Receive the response ---
    // We read bytes into a buffer with a 200ms overall timeout.
    // A 30ms gap after the last received byte indicates end-of-frame
    // (Modbus RTU uses 3.5 character times of silence as frame delimiter;
    // at 19200 baud, one character ≈ 0.57ms, so 30ms is very conservative).
    uint8_t response[32];
    int bytesReceived = 0;
    unsigned long startTime = millis();
    unsigned long lastByteTime = 0;

    while ((millis() - startTime) < 200) {  // 200ms total timeout
        if (ModbusSerial.available()) {
            response[bytesReceived] = ModbusSerial.read();
            lastByteTime = millis();
            bytesReceived++;
            if (bytesReceived >= 32) break;  // Buffer full, stop reading
        }
        // If we've received at least one byte and 30ms has passed since
        // the last byte, assume the frame is complete
        if (lastByteTime > 0 && (millis() - lastByteTime) > 30) break;
    }

    // --- Detect and skip TX echo ---
    // The RS-485-to-TTL converter echoes our 8-byte request back to us.
    // We detect this by comparing the first 8 received bytes to our request.
    // If they match, the actual response starts at byte index 8.
    int start = 0;
    if (bytesReceived >= 8) {
        bool isEcho = true;
        for (int i = 0; i < 8; i++) {
            if (response[i] != request[i]) { isEcho = false; break; }
        }
        if (isEcho) start = 8;  // Skip the echo, real response starts after
    }

    // --- Parse the actual Modbus response ---
    int len = bytesReceived - start;    // Length of actual response
    uint8_t *resp = &response[start];   // Pointer to start of actual response

    // Valid response format: [SlaveID][0x03][ByteCount=0x02][DataHi][DataLo][CRC_Lo][CRC_Hi]
    // Total: 7 bytes for a single-register read
    if (len >= 7 && resp[0] == MODBUS_SLAVE_ID && resp[1] == 0x03 && resp[2] == 0x02) {
        // Verify CRC of the response (calculated over first 5 bytes)
        uint16_t recvCrc = (resp[6] << 8) | resp[5];  // Extract received CRC
        uint16_t calcCrc = crc16(resp, 5);             // Calculate expected CRC
        if (recvCrc == calcCrc) {
            // CRC valid — extract the 16-bit register value
            return (resp[3] << 8) | resp[4];  // Data high byte | Data low byte
        }
        return -2;  // CRC mismatch — data corruption on the wire
    }

    // Check for Modbus exception response
    // Exception format: [SlaveID][0x83][ExceptionCode][CRC_Lo][CRC_Hi]
    // Function code 0x83 = 0x80 + 0x03 (error flag + original function code)
    if (len >= 5 && resp[0] == MODBUS_SLAVE_ID && resp[1] == 0x83) {
        return -1000 - resp[2];  // Return negative error code for diagnostics
    }

    return -1;  // No valid response received (timeout or garbled data)
}

// =============================================================================
// READ DISTANCE FROM BANNER LE250 SENSOR
// =============================================================================
// The Banner LE250 distance measurement is a 32-bit value stored across two
// consecutive 16-bit Modbus registers on the R45C IO-Link master:
//   Register 41002 (0x03E9): High 16 bits of raw distance
//   Register 41003 (0x03EA): Low 16 bits of raw distance
//
// The raw 32-bit value is divided by 8000.0 to convert to millimeters.
// This scaling factor was determined empirically during development.
//
// A 15ms delay between the two register reads ensures the R45C has time
// to process the second request (Modbus devices need inter-frame gaps).
//
// Returns:
//   >= 0.0: Distance in millimeters
//   -1.0:   Read error (sensor offline or communication failure)
// =============================================================================
float readDistance() {
    // Read the high word (upper 16 bits of the 32-bit distance value)
    int32_t regHigh = readRegister(REG_PDI_HIGH);
    delay(15);  // Inter-frame delay to allow R45C to prepare for next request

    // Read the low word (lower 16 bits of the 32-bit distance value)
    int32_t regLow = readRegister(REG_PDI_LOW);

    // Only proceed if both reads were successful (non-negative return values)
    if (regHigh >= 0 && regLow >= 0) {
        // Combine the two 16-bit values into a single 32-bit unsigned integer:
        //   High word shifted left by 16 bits, OR'd with the low word
        //   Example: regHigh=0x0012, regLow=0x3456 → combined=0x00123456
        uint32_t combined = ((uint32_t)(uint16_t)regHigh << 16) | (uint16_t)regLow;

        // Convert raw value to millimeters using empirically determined scale factor
        return combined / 8000.0;
    }

    return -1.0;  // Indicate read failure
}

// =============================================================================
// ESP-NOW SEND CALLBACK
// =============================================================================
// Called automatically by the ESP-NOW stack after each transmission attempt.
// Updates the global status flag indicating whether the receiver acknowledged
// the packet. This runs in interrupt context, so only simple operations are safe.
//
// Parameters:
//   mac_addr — MAC address of the intended recipient
//   status   — ESP_NOW_SEND_SUCCESS or ESP_NOW_SEND_FAIL
// =============================================================================
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
}

// =============================================================================
// SETUP — Runs once at power-on or reset
// =============================================================================
void setup() {
    // Initialize USB serial for debug output (visible in PlatformIO serial monitor)
    Serial.begin(115200);
    delay(2000);  // Allow time for serial monitor to connect after USB enumeration

    // Print startup banner for identification
    Serial.println("\n====================================================");
    Serial.printf("   SENSOR NODE %d (Dashboard: Sensor %d)\n", NODE_ID, NODE_ID + 1);
    Serial.println("   Modbus RTU -> ESP-NOW Sender");
    Serial.println("====================================================\n");

    // --- Initialize Modbus UART ---
    // Configure UART1 for RS-485 communication with the R45C.
    // INPUT_PULLUP on RX prevents floating pin noise when no data is being received.
    // 8N1 = 8 data bits, no parity, 1 stop bit (standard Modbus RTU framing).
    pinMode(RX_PIN, INPUT_PULLUP);
    ModbusSerial.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
    delay(100);  // Allow UART peripheral to stabilize

    // Flush any garbage bytes that may be in the buffer from power-on noise
    while (ModbusSerial.available()) ModbusSerial.read();
    Serial.println("Modbus initialized (RX:18, TX:17, 19200 baud)");

    // --- Initialize WiFi in Station mode for ESP-NOW ---
    // ESP-NOW requires WiFi to be initialized but does not need an active connection.
    // Station mode (WIFI_STA) is used because the receiver runs in AP+STA mode,
    // and ESP-NOW communication works between these modes.
    WiFi.mode(WIFI_STA);
    Serial.printf("Sender MAC: %s\n", WiFi.macAddress().c_str());

    // --- Initialize ESP-NOW protocol ---
    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: ESP-NOW init failed!");
        return;  // Cannot proceed without ESP-NOW
    }

    // Register the send callback to track delivery status
    esp_now_register_send_cb(onDataSent);

    // --- Register the receiver as an ESP-NOW peer ---
    // ESP-NOW requires explicitly adding each device you want to communicate with.
    // Channel 0 means "use the current channel" (auto-match with receiver's AP channel).
    // Encryption is disabled for simplicity and lower latency.
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMAC, 6);
    peerInfo.channel = 0;       // Auto-match channel
    peerInfo.encrypt = false;   // No encryption (faster, simpler)

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("ERROR: Failed to add receiver peer!");
        return;
    }
    Serial.printf("Receiver peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 receiverMAC[0], receiverMAC[1], receiverMAC[2],
                 receiverMAC[3], receiverMAC[4], receiverMAC[5]);

    // --- Perform initial sensor test ---
    // Attempt one distance reading to verify the hardware chain is working
    // (Banner LE250 → R45C → RS-485 → ESP32). Prints a warning if it fails,
    // but does not halt — the sensor may come online after warm-up.
    Serial.println("\nTesting sensor...");
    float test = readDistance();
    if (test >= 0) {
        Serial.printf("Sensor OK! Distance: %.2f mm\n", test);
    } else {
        Serial.println("WARNING: Could not read sensor. Check wiring.");
    }

    Serial.println("\nSystem ready. Transmitting...\n");
}

// =============================================================================
// MAIN LOOP — Runs continuously after setup()
// =============================================================================
// The loop reads the sensor and transmits data at a fixed interval (20 Hz).
// Static variables persist across loop iterations to track timing and counts.
// =============================================================================
void loop() {
    static uint32_t lastSend = 0;   // Timestamp of last transmission (for interval timing)
    static uint32_t txCount = 0;    // Total successful transmissions (for diagnostics)
    static uint32_t errCount = 0;   // Total sensor read errors (for diagnostics)

    // --- Check if it's time to read and transmit ---
    // Using millis() subtraction handles unsigned integer overflow correctly.
    if (millis() - lastSend >= SEND_INTERVAL_MS) {
        lastSend = millis();

        // --- Read distance from the Banner LE250 via Modbus ---
        float distance = readDistance();

        if (distance >= 0) {
            // --- Build the ESP-NOW packet ---
            sensor_packet_t packet;
            packet.nodeId = NODE_ID;          // Identify which corner this is
            packet.distance_mm = distance;    // Current distance reading in mm
            packet.timestamp = millis();      // When this reading was taken

            // --- Transmit via ESP-NOW ---
            // esp_now_send() queues the packet for transmission and returns
            // immediately. The onDataSent callback fires later with the result.
            esp_err_t result = esp_now_send(receiverMAC,
                                            (uint8_t*)&packet,
                                            sizeof(sensor_packet_t));

            if (result == ESP_OK) {
                txCount++;
                // Print periodic status (first 3 packets, then every 40th)
                // to avoid flooding the serial monitor at 20 Hz
                if (txCount % 40 == 0 || txCount <= 3) {
                    Serial.printf("TX #%lu: %.2f mm\n", txCount, distance);
                }
            }
        } else {
            // --- Sensor read failed ---
            // This can happen if the R45C is still booting, the wiring is loose,
            // or the Banner LE250 is out of range / obstructed.
            errCount++;
            // Print sparingly to avoid serial spam during persistent errors
            if (errCount <= 3 || errCount % 100 == 0) {
                Serial.printf("Sensor error #%lu\n", errCount);
            }
        }
    }
}