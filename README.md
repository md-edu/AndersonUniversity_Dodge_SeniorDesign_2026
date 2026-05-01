# CNC Machine Leveling System — ESP32-S3 Sensor Network

A wireless sensor network that measures CNC machine bed levelness using Banner LE250 laser distance sensors, IO-Link to Modbus RTU conversion, ESP-NOW communication, and a real-time web dashboard served from an ESP32-S3 captive portal.

---

## Table of Contents

1. [System Overview](#system-overview)
2. [Repository Structure](#repository-structure)
3. [Assembly Instructions](#assembly-instructions)
4. [Operating Instructions](#operating-instructions)
5. [Software Architecture](#software-architecture)
6. [Lessons Learned](#lessons-learned)

---

## System Overview

This system measures the distance between a CNC machine bed and a fixed reference point at up to four corners using Banner LE250 laser distance sensors. Each sensor node reads distance data via Modbus RTU and transmits it wirelessly to a central receiver using ESP-NOW. The receiver hosts a WiFi access point with a captive portal that serves a live web dashboard, allowing any device with a browser to monitor all four sensor readings in real time.

**Hardware per sensor node:**
- 1× Banner LE250 laser distance sensor
- 1× Banner R45C IO-Link master (converts IO-Link to Modbus RTU)
- 1× RS-485 to TTL converter module
- 1× ESP32-S3 development board
- 1× 24V battery (powers the R45C and Banner LE250)
- 1× 5V power source for the ESP32-S3

**Hardware for the central receiver:**
- 1× ESP32-S3 development board
- 1× 5V power source (USB or battery)

---

## Repository Structure

```
├── receiver/                    # Central receiver / web dashboard
│   ├── src/
│   │   └── main.cpp            # ESP-NOW receiver, WiFi AP, captive portal, REST API
│   ├── data/                   # Web dashboard files (uploaded to LittleFS)
│   │   ├── index.html          # Dashboard HTML layout
│   │   ├── style.css           # Dashboard styling and color-coded sensor boxes
│   │   ├── script.js           # Polls /api/data every second, updates UI
│   │   ├── logo.png            # Company logo displayed on dashboard
│   │   └── building.png        # Background image for dashboard
│   └── platformio.ini          # PlatformIO build configuration
│
├── sender/                     # Sensor node (one per corner)
│   ├── src/
│   │   └── main.cpp            # Modbus RTU reader, ESP-NOW transmitter
│   └── platformio.ini          # PlatformIO build configuration
│
└── README.md                   # This file
```

---

## Assembly Instructions

### Sensor Node Wiring

Each sensor node consists of a Banner LE250 sensor connected to a Banner R45C IO-Link master, which is then connected to an ESP32-S3 via an RS-485 to TTL converter.

#### Step 1: Banner LE250 to R45C IO-Link Master

Connect the Banner LE250 directly to the R45C IO-Link master using the M12 sensor cable. The R45C handles the IO-Link protocol from the LE250 and exposes the sensor data over Modbus RTU via its RS-485 interface.

#### Step 2: R45C to RS-485-to-TTL Converter

| R45C Wire Color | Connection                          |
|-----------------|-------------------------------------|
| Red             | 24V battery positive terminal       |
| Blue            | 24V battery negative terminal (GND) |
| Black           | RS-485-to-TTL module A+ terminal    |
| White           | RS-485-to-TTL module B- terminal    |

#### Step 3: RS-485-to-TTL Converter to ESP32-S3

| RS-485-to-TTL Pin | ESP32-S3 Pin       |
|--------------------|---------------------|
| RX                 | GPIO 18 (UART RX)   |
| TX                 | GPIO 17 (UART TX)   |
| VCC                | 3.3V output          |
| GND                | GND                  |

#### Step 4: ESP32-S3 Power

Connect the ESP32-S3 to a 5V power source (USB power bank or regulated 5V supply) and ensure the ESP32-S3 GND is connected to the common ground shared with the RS-485-to-TTL converter.

#### Step 5: Verify Connections

Before powering on, verify the following:
- The 24V battery powers the R45C (red = +24V, blue = GND).
- The R45C RS-485 data lines connect to the RS-485-to-TTL converter (black = A+, white = B-).
- The RS-485-to-TTL converter connects to the ESP32-S3 UART (RX to GPIO 18, TX to GPIO 17, VCC to 3.3V, GND to GND).
- The ESP32-S3 is powered from a 5V source with a shared ground.

### Central Receiver

The receiver ESP32-S3 requires no additional wiring beyond a 5V power source (USB). It communicates wirelessly with the sender nodes via ESP-NOW and hosts the web dashboard over WiFi.

### Firmware Upload

This project uses PlatformIO. To flash the firmware and web dashboard files:

#### Sender Node

```bash
cd sender
pio run --target upload
```

#### Receiver Node

```bash
cd receiver
pio run --target upload          # Upload firmware
pio run --target uploadfs        # Upload web dashboard files to LittleFS
```

The `uploadfs` step is critical. Without it, the web dashboard files will not be present on the ESP32-S3 filesystem and the dashboard will not load.

### Configuring Multiple Sensor Nodes

Each sensor node must have a unique `NODE_ID` defined in `sender/src/main.cpp`:

```cpp
#define NODE_ID 0   // Change to 0, 1, 2, or 3 for each corner
```

- `NODE_ID 0` → Sensor 1 on the dashboard
- `NODE_ID 1` → Sensor 2 on the dashboard
- `NODE_ID 2` → Sensor 3 on the dashboard
- `NODE_ID 3` → Sensor 4 on the dashboard

Each sender must also have the correct receiver MAC address set in `receiverMAC[]`. The receiver must have each sender's MAC address registered as a peer (currently configured for one sender in `senderMAC[]`).

---

## Operating Instructions

### Step 1: Power On the Receiver

Connect the receiver ESP32-S3 to a 5V power source. It will automatically start a WiFi access point and begin listening for ESP-NOW packets. The serial monitor (115200 baud) will display:

```
====================================================
   ESP32-S3 Central Receiver
   ESP-NOW -> Web Dashboard
====================================================
AP SSID: SensorHub-Center
AP Password: sensor123
AP IP: 192.168.4.1
```

### Step 2: Power On the Sensor Node(s)

Connect each sender ESP32-S3 to its 5V power source. Ensure the 24V battery is connected to the R45C. The sender will immediately begin reading the Banner LE250 sensor via Modbus RTU and transmitting distance data to the receiver at 20 Hz. The serial monitor will display:

```
====================================================
   SENSOR NODE 0 (Dashboard: Sensor 1)
   Modbus RTU -> ESP-NOW Sender
====================================================
Sensor OK! Distance: 123.45 mm
System ready. Transmitting...
```

### Step 3: Connect to the Dashboard

On any WiFi-enabled device (phone, tablet, laptop):

1. Open WiFi settings and connect to the network **SensorHub-Center** with password **sensor123**.
2. A captive portal should automatically open showing the dashboard. If it does not, open a browser and navigate to **http://192.168.4.1**.
3. The dashboard displays a 2×2 grid of sensor boxes. Each box shows the live distance reading in millimeters. If a sensor node is not transmitting, its box will display "NO SIGNAL."

### Step 4: Interpreting the Dashboard

- Each sensor box corresponds to one corner of the CNC machine bed.
- Distance values are displayed in millimeters and update every second.
- If a sensor node stops transmitting for more than 3 seconds, it is marked offline and displays "NO SIGNAL."
- The clock in the upper-left corner shows the current time from the connected device's browser.

---

## Software Architecture

The system consists of two firmware applications and a web frontend. The following sections describe the purpose and structure of each component. All source files contain detailed inline comments explaining the logic.

### Sender Firmware (`sender/src/main.cpp`)

The sender firmware runs on each sensor node ESP32-S3. Its responsibilities are:

1. **Modbus RTU Communication:** Reads two 16-bit holding registers (41002 and 41003) from the R45C IO-Link master over RS-485 at 19200 baud. These two registers contain the high and low words of the Banner LE250 distance measurement. The firmware implements raw Modbus RTU framing with CRC-16 calculation, request construction, response parsing, and TX echo detection (the RS-485-to-TTL converter echoes transmitted bytes back on the RX line, which must be stripped from the response).

2. **Distance Calculation:** The two 16-bit register values are combined into a 32-bit unsigned integer and divided by 8000.0 to produce the distance in millimeters.

3. **ESP-NOW Transmission:** Every 50 milliseconds (20 Hz), the firmware packages the distance reading into a 9-byte packed struct (`sensor_packet_t`) containing the node ID, distance in millimeters, and a timestamp, then transmits it to the receiver via ESP-NOW.

### Receiver Firmware (`receiver/src/main.cpp`)

The receiver firmware runs on the central ESP32-S3. Its responsibilities are:

1. **ESP-NOW Reception:** Receives `sensor_packet_t` packets from up to four sender nodes. Each packet is validated by size and node ID, then stored in a `NodeState` array. Nodes that have not sent data within 3 seconds are marked offline.

2. **WiFi Access Point:** Hosts a WiFi network ("SensorHub-Center") that any device can connect to. The ESP32-S3 runs in `WIFI_AP_STA` mode to support both the access point and ESP-NOW simultaneously.

3. **Captive Portal:** A DNS server intercepts all DNS queries and redirects them to the ESP32-S3's IP address (192.168.4.1). Common captive portal detection URLs used by Android, iOS, and Windows are explicitly handled to ensure the dashboard opens automatically when a device connects.

4. **Web Server and REST API:** Serves the dashboard files from LittleFS and exposes two JSON API endpoints:
   - `/api/data` — Returns the current state of all four sensor nodes (online status, distance, timestamps, packet count).
   - `/api/status` — Returns system information (SSID, IP, MAC, uptime, connected clients).

5. **LittleFS Filesystem:** The web dashboard files (HTML, CSS, JavaScript, images) are stored in the ESP32-S3's flash memory using the LittleFS filesystem and served to connected clients by the web server.

### Web Dashboard (`receiver/data/`)

The web dashboard is a single-page application consisting of three files:

- **`index.html`** — Defines the dashboard layout: a title, date header, and a 2×2 grid of sensor boxes. Each box displays a sensor label and its current distance reading.

- **`style.css`** — Provides visual styling including a background image with a semi-transparent overlay, color-coded sensor boxes (green for good, yellow for warning, red for bad), a fixed clock in the upper-left corner, and a fade-in logo animation.

- **`script.js`** — Polls the `/api/data` endpoint every second using the Fetch API. When data is received, it updates each sensor box with the current distance value in millimeters. If a node is offline, the box displays "NO SIGNAL." The script also maintains a live clock using the browser's local time.

### Data Flow

```
Banner LE250  →  R45C (IO-Link to Modbus)  →  RS-485-to-TTL  →  ESP32-S3 Sender
                                                                        │
                                                                   ESP-NOW (20 Hz)
                                                                        │
                                                                        ▼
                                                                ESP32-S3 Receiver
                                                                   │         │
                                                              WiFi AP    REST API
                                                                   │         │
                                                                   ▼         ▼
                                                              Browser ← /api/data
                                                              (Dashboard updates every 1s)
```

### Communication Protocol

The sender and receiver share a packed C struct for ESP-NOW communication:

```cpp
typedef struct __attribute__((packed)) {
    uint8_t  nodeId;        // 0-3 maps to Sensor 1-4
    float    distance_mm;   // Distance in millimeters
    uint32_t timestamp;     // millis() on sender
} sensor_packet_t;
```

This struct is 9 bytes and is transmitted as raw bytes over ESP-NOW. Both the sender and receiver must define this struct identically.

---

## Lessons Learned

### Interfacing IO-Link Devices with an ESP32

The most significant challenge in this project was interfacing the Banner LE250 IO-Link sensor with the ESP32-S3 microcontroller. The LE250 communicates natively over IO-Link, which is not directly supported by the ESP32. The solution was to use the Banner R45C IO-Link master as a protocol bridge. The R45C converts IO-Link data from the sensor into Modbus RTU registers accessible over RS-485. An RS-485-to-TTL converter module then level-shifts the RS-485 signals to 3.3V TTL levels compatible with the ESP32-S3's UART.

### Discovering the Correct Modbus Registers

The Banner LE250 distance data is stored across two 16-bit Modbus holding registers (41002 and 41003) on the R45C. These register addresses were not immediately obvious from the documentation. The team systematically scanned approximately 300 registers to identify which ones contained live distance data that changed in response to sensor movement. The two register values are combined into a single 32-bit integer and divided by 8000.0 to produce the distance in millimeters.

### RS-485 TX Echo Handling

The RS-485-to-TTL converter module used in this project echoes transmitted bytes back on the RX line because the DE (Driver Enable) and RE (Receiver Enable) pins are tied together for auto-direction control. This means that every Modbus request sent by the ESP32 appears at the beginning of the response buffer. The sender firmware detects and strips this echo by comparing the first 8 bytes of the response to the original request before parsing the actual Modbus response.

### Divide and Conquer Debugging

When the full system was not working end-to-end, the team adopted a divide-and-conquer debugging strategy. Each subsystem (Modbus communication, ESP-NOW transmission, WiFi access point, web dashboard) was tested independently before integrating them together. This approach made it possible to isolate issues to specific components rather than debugging the entire system at once.

### LittleFS Filesystem Upload

The web dashboard files must be uploaded to the ESP32-S3's LittleFS partition separately from the firmware. Forgetting to run `pio run --target uploadfs` after modifying dashboard files was a recurring source of confusion during development, as the firmware would compile and upload successfully but the dashboard would fail to load in the browser.