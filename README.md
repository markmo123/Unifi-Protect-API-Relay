# UniFi Protect Sensor Bridge — NodeMCU

An Arduino sketch for the **NodeMCU (ESP8266)** that polls a [UniFi Protect](https://ui.com/camera-security) sensor for temperature, humidity, and light readings, forwards them to a backend HTTPS API, and displays the latest values on a **128×64 I2C OLED**.

---

## How it works

```
UniFi Console  ──GET──►  NodeMCU  ──POST──►  Backend API
(local HTTPS)                │
                             └──────────►  SSD1306 OLED
```

Every 15 seconds (configurable) the sketch:

1. GETs the sensor state from the UniFi Protect Integration API
2. Parses temperature (°C), humidity (%), light (lux), battery %, and connection state from the JSON response
3. POSTs `{ temperature, humidity, light }` to your backend API
4. Updates the OLED display with the latest readings and a POST status indicator

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | NodeMCU 1.0 (ESP-12E / ESP8266) |
| Display | 0.96″ SSD1306 128×64 OLED, I2C |
| Sensor | UniFi Access Sensor (G5 Sensor or compatible) |

### OLED wiring

| OLED pin | NodeMCU pin |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | D2 (GPIO 4) |
| SCL | D1 (GPIO 5) |

> If the display stays blank, change `OLED_I2C_ADDR` in the sketch from `0x3C` to `0x3D`.

---

## Prerequisites

### Arduino IDE setup

1. In **Preferences → Additional Board URLs**, add:
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
2. Open **Tools → Board → Board Manager**, search for `esp8266`, and install **esp8266 by ESP8266 Community**.
3. Select **Tools → Board → NodeMCU 1.0 (ESP-12E Module)**.

### Libraries

Install all of the following via **Sketch → Include Library → Manage Libraries**:

| Library | Author | Notes |
|---|---|---|
| `ESP8266WiFi` | ESP8266 Community | Bundled with the board package |
| `ESP8266HTTPClient` | ESP8266 Community | Bundled with the board package |
| `WiFiClientSecure` | ESP8266 Community | Bundled with the board package |
| `ArduinoJson` | Benoit Blanchon | Install **v6.x** |
| `Adafruit SSD1306` | Adafruit | |
| `Adafruit GFX Library` | Adafruit | |

---

## Configuration

Open `nodemcu_unifi_protect.ino` and edit the constants at the top of the file:

```cpp
// Wi-Fi
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// UniFi Protect
const char* PROTECT_HOST    = "192.168.1.1";       // Console LAN IP
const char* SENSOR_ID       = "YOUR_SENSOR_ID";    // See below
const char* PROTECT_API_KEY = "YOUR_UNIFI_API_KEY";

// Backend API
const char* POST_API_URL    = "https://your-functions-url/api/readings";
const char* POST_API_KEY    = "YOUR_FUNCTIONS_API_KEY";

// Poll interval
const unsigned long POLL_INTERVAL_MS = 15000;      // 15 seconds
```

### Finding your Sensor ID

The easiest way is via the Serial Monitor:

1. Temporarily change `SENSOR_ID` to an empty string `""` and point the GET URL at the list endpoint by editing `buildGetUrl()` to return `"https://" + String(PROTECT_HOST) + "/proxy/protect/integration/v1/sensors"`.
2. Flash and open Serial Monitor at **115200 baud**.
3. Copy the `"id"` value for your sensor from the printed JSON.
4. Paste it back into `SENSOR_ID` and revert `buildGetUrl()`.

Alternatively, the sensor ID appears in the URL when you open the sensor's detail page in the UniFi Protect web UI.

### Generating a UniFi API Key

1. Log in to **UniFi OS** on your console.
2. Go to **Settings → API Keys**.
3. Create a new key — **read-only** access is sufficient.

---

## API reference

### GET — UniFi Protect sensor

```
GET https://<PROTECT_HOST>/proxy/protect/integration/v1/sensors/<SENSOR_ID>
X-API-Key: <PROTECT_API_KEY>
```

Relevant fields parsed from the response:

```json
{
  "state": "CONNECTED",
  "stats": {
    "temperature": { "value": 22.5 },
    "humidity":    { "value": 58.3 },
    "light":       { "value": 412  }
  },
  "batteryStatus": { "percentage": 87 }
}
```

> ⚠️ UniFi consoles use self-signed TLS certificates. The sketch calls `setInsecure()` to skip certificate verification. Only use this on a trusted local network.

### POST — Backend API

```
POST https://<your-functions-url>/api/readings
x-api-key: <POST_API_KEY>
Content-Type: application/json

{
  "temperature": 22.5,
  "humidity": 58.3,
  "light": 412
}
```

---

## OLED display layout

```
┌────────────────────────────┐
│ UniFi Protect    POST OK   │  ← inverted header bar
│ Temp:  22.5 C              │
│ Hum:   58.3 %              │
│ Lux:412  Bat:87%         ● │  ← ● = connected  ○ = disconnected
└────────────────────────────┘
```

The small circle in the bottom-right corner reflects the sensor's `state` field — solid when `CONNECTED`, hollow otherwise.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| OLED stays blank | Verify wiring; try `OLED_I2C_ADDR 0x3D` |
| Wi-Fi timeout on boot | Confirm SSID/password; device auto-restarts after 20 s |
| `GET failed` in Serial Monitor | Check `PROTECT_HOST`, `SENSOR_ID`, and API key; ensure the NodeMCU is on the same LAN as the console |
| `POST failed` | Verify `POST_API_URL` and `POST_API_KEY`; check your function/backend logs |
| JSON parse error | Open Serial Monitor and inspect the raw payload; the sensor may be offline (`state: DISCONNECTED`) |
| `GET response code: -1` | TLS handshake issue — the ESP8266 can struggle with some TLS 1.3 servers; try forcing TLS 1.2 on your backend if possible |

---

## License

MIT — free to use, modify, and distribute.
