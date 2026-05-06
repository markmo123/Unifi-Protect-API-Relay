/*
 * NodeMCU – UniFi Protect Sensor Bridge
 * ----------------------------------------
 * - GETs a single sensor from the UniFi Protect Integration API
 *   GET https://<CONSOLE_IP>/proxy/protect/integration/v1/sensors/<SENSOR_ID>
 * - Parses temperature, humidity, and light (lux) from the nested JSON response
 * - POSTs { temperature, humidity, light } to a backend HTTPS API
 *   using an x-api-key header
 * - Displays the last received readings on a 128×64 I2C OLED (SSD1306)
 *
 * Wiring:
 *   OLED SDA  -> D2 (GPIO4)
 *   OLED SCL  -> D1 (GPIO5)
 *   OLED VCC  -> 3.3V
 *   OLED GND  -> GND
 *
 * Libraries required (install via Arduino Library Manager):
 *   - ESP8266WiFi           (bundled with ESP8266 board package)
 *   - ESP8266HTTPClient     (bundled with ESP8266 board package)
 *   - WiFiClientSecure      (bundled with ESP8266 board package)
 *   - ArduinoJson           by Benoit Blanchon  (v6.x)
 *   - Adafruit SSD1306      by Adafruit
 *   - Adafruit GFX Library  by Adafruit
 *
 * Board: NodeMCU 1.0 (ESP-12E Module)
 *
 * ⚠ UniFi consoles use self-signed TLS certificates. This sketch disables
 *   certificate verification (setInsecure). Use only on a trusted LAN.
 *
 * UniFi Protect API response structure (relevant fields):
 * {
 *   "id": "...",
 *   "state": "CONNECTED",
 *   "stats": {
 *     "temperature": { "value": 22.5 },   // °C
 *     "humidity":    { "value": 58.3 },   // %
 *     "light":       { "value": 412  }    // lux
 *   },
 *   "batteryStatus": { "percentage": 87 }
 * }
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>   // ← HTTPS support
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── USER CONFIGURATION ────────────────────────────────────────────────────────

// Wi-Fi credentials
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// UniFi Protect console (local IP or hostname – no trailing slash)
const char* PROTECT_HOST  = "192.168.1.1";

// UniFi Protect sensor ID (find it in the Protect UI → sensor details URL,
// or call GET /proxy/protect/integration/v1/sensors to list all)
const char* SENSOR_ID     = "YOUR_SENSOR_ID";

// API key – generate in UniFi OS → Settings → API Keys (read-only is sufficient)
const char* PROTECT_API_KEY = "YOUR_UNIFI_API_KEY";

// Full GET URL (assembled at runtime from the constants above)
// https://<PROTECT_HOST>/proxy/protect/integration/v1/sensors/<SENSOR_ID>

// POST endpoint – Azure / cloud Functions URL
// e.g. "https://my-app.azurewebsites.net/api/readings"
const char* POST_API_URL = "https://your-functions-url/api/readings";

// x-api-key sent as a header to the POST endpoint
const char* POST_API_KEY = "YOUR_FUNCTIONS_API_KEY";

// How often to poll & push (milliseconds)
const unsigned long POLL_INTERVAL_MS = 15000;  // 15 seconds

// ─── OLED ──────────────────────────────────────────────────────────────────────

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_I2C_ADDR 0x3C  // Try 0x3D if display stays blank

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── GLOBALS ───────────────────────────────────────────────────────────────────

float  g_temperature  = 0.0;
float  g_humidity     = 0.0;
float  g_light        = 0.0;
int    g_battery      = -1;     // -1 = unknown
String g_sensorState  = "";     // CONNECTED / DISCONNECTED / etc.
bool   g_hasData      = false;
String g_statusMsg    = "Starting...";

unsigned long g_lastPoll = 0;

// ─── FORWARD DECLARATIONS ──────────────────────────────────────────────────────

bool fetchSensorData();
bool postSensorData();
void updateDisplay();
void showStatus(const String& line1, const String& line2 = "");
String buildGetUrl();

// ───────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\nUniFi Protect Sensor Bridge starting...");

  // ── OLED ───────────────────────────────────────────────────────────────────
  Wire.begin(4, 5);  // SDA=D2(GPIO4), SCL=D1(GPIO5)
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println("ERROR: SSD1306 not found. Check wiring / I2C address.");
    while (true) { delay(500); }
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  showStatus("UniFi Protect", "Sensor Bridge");
  delay(1500);

  // ── Wi-Fi ──────────────────────────────────────────────────────────────────
  showStatus("Connecting WiFi...", WIFI_SSID);
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 20000) {
      Serial.println("\nWi-Fi timeout. Restarting...");
      showStatus("WiFi timeout!", "Restarting...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println();
  Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  showStatus("WiFi OK", WiFi.localIP().toString());
  delay(1000);
}

// ───────────────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost – reconnecting...");
    showStatus("WiFi lost...", "Reconnecting");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  unsigned long now = millis();
  if (g_lastPoll == 0 || now - g_lastPoll >= POLL_INTERVAL_MS) {
    g_lastPoll = now;

    showStatus("Fetching...", "UniFi Protect");

    if (fetchSensorData()) {
      Serial.printf(
        "Sensor: state=%s  temp=%.1f°C  hum=%.1f%%  light=%.0flux  bat=%d%%\n",
        g_sensorState.c_str(), g_temperature, g_humidity, g_light, g_battery
      );

      showStatus("Posting data...", "");

      if (postSensorData()) {
        g_statusMsg = "POST OK";
        Serial.println("POST succeeded.");
      } else {
        g_statusMsg = "POST failed";
        Serial.println("POST failed.");
      }

      g_hasData = true;
    } else {
      g_statusMsg = "GET failed";
      Serial.println("Sensor fetch failed.");
    }

    updateDisplay();
  }
}

// ───────────────────────────────────────────────────────────────────────────────
// Build the full HTTPS URL for the Protect sensor endpoint
// ───────────────────────────────────────────────────────────────────────────────
String buildGetUrl() {
  String url = "https://";
  url += PROTECT_HOST;
  url += "/proxy/protect/integration/v1/sensors/";
  url += SENSOR_ID;
  return url;
}

// ───────────────────────────────────────────────────────────────────────────────
// GET sensor data from UniFi Protect
// Parses the nested stats.temperature.value / stats.humidity.value /
// stats.light.value fields from the API response.
// Returns true on success.
// ───────────────────────────────────────────────────────────────────────────────
bool fetchSensorData() {
  WiFiClientSecure client;
  client.setInsecure();   // Skip cert verification – UniFi uses self-signed certs

  HTTPClient http;
  String url = buildGetUrl();
  Serial.print("GET ");
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println("http.begin() failed for GET");
    return false;
  }

  http.setTimeout(10000);
  http.addHeader("X-API-Key", PROTECT_API_KEY);
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  Serial.printf("GET response code: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("GET error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  // Debug – comment out if the payload is large
  Serial.println("--- Payload ---");
  Serial.println(payload);
  Serial.println("---------------");

  // The Protect sensor JSON can be large; use a generous filter to extract only
  // what we need and keep heap usage low on the ESP8266.
  //
  // Full path: stats → temperature/humidity/light → value
  //            batteryStatus → percentage
  //            state
  StaticJsonDocument<256> filter;
  filter["state"]                      = true;
  filter["stats"]["temperature"]["value"] = true;
  filter["stats"]["humidity"]["value"]    = true;
  filter["stats"]["light"]["value"]       = true;
  filter["batteryStatus"]["percentage"]   = true;

  // Use a larger doc for the filtered parse (filtered responses are small)
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  // Validate the key fields exist
  if (!doc["stats"]["temperature"]["value"].is<float>() &&
      !doc["stats"]["temperature"]["value"].is<int>()) {
    Serial.println("JSON missing stats.temperature.value – check sensor ID / API key.");
    return false;
  }

  g_sensorState = doc["state"] | "UNKNOWN";
  g_temperature = doc["stats"]["temperature"]["value"].as<float>();
  g_humidity    = doc["stats"]["humidity"]["value"].as<float>();
  g_light       = doc["stats"]["light"]["value"].as<float>();
  g_battery     = doc["batteryStatus"]["percentage"] | -1;

  return true;
}

// ───────────────────────────────────────────────────────────────────────────────
// POST the latest readings to the backend API
// Body:   { "temperature": 23.4, "humidity": 58.2, "light": 420 }
// Header: x-api-key: <POST_API_KEY>
// Returns true on HTTP 200 or 201.
// ───────────────────────────────────────────────────────────────────────────────
bool postSensorData() {
  // POST endpoint is always HTTPS
  WiFiClientSecure secureClient;
  secureClient.setInsecure();   // POST target may also use a managed/trusted cert;
                                // setInsecure keeps things simple — swap in a
                                // fingerprint or CA cert if you need strict validation

  HTTPClient http;

  if (!http.begin(secureClient, POST_API_URL)) {
    Serial.println("http.begin() failed for POST");
    return false;
  }

  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", POST_API_KEY);

  // Payload matches the API spec exactly – no extra fields
  StaticJsonDocument<96> doc;
  doc["temperature"] = serialized(String(g_temperature, 1));
  doc["humidity"]    = serialized(String(g_humidity, 1));
  doc["light"]       = (int)g_light;

  String body;
  serializeJson(doc, body);
  Serial.print("POST body: ");
  Serial.println(body);

  int httpCode = http.POST(body);
  http.end();

  Serial.printf("POST response code: %d\n", httpCode);
  return (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
}

// ───────────────────────────────────────────────────────────────────────────────
// Render the latest values on the OLED
// Layout:
//   ┌─────────────────────────┐
//   │ UniFi Protect  POST OK  │  ← header bar (inverted)
//   │ Temp:  22.5 °C          │
//   │ Hum:   58.3 %           │
//   │ Light: 412 lux  Bat:87% │
//   └─────────────────────────┘
// ───────────────────────────────────────────────────────────────────────────────
void updateDisplay() {
  display.clearDisplay();

  // ── Header ─────────────────────────────────────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2);
  display.print("UniFi Protect");

  // Right-align status string in the header
  int16_t  sx, sy;
  uint16_t sw, sh;
  display.getTextBounds(g_statusMsg, 0, 0, &sx, &sy, &sw, &sh);
  display.setCursor(SCREEN_WIDTH - (int)sw - 2, 2);
  display.print(g_statusMsg);

  display.setTextColor(SSD1306_WHITE);

  if (!g_hasData) {
    display.setTextSize(1);
    display.setCursor(10, 28);
    display.print("Waiting for data...");
    display.display();
    return;
  }

  char buf[24];

  // ── Temperature ────────────────────────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 16);
  display.print("Temp:");
  display.setTextSize(2);
  display.setCursor(42, 13);
  snprintf(buf, sizeof(buf), "%.1f", g_temperature);
  display.print(buf);
  display.setTextSize(1);
  display.print(" C");

  // ── Humidity ───────────────────────────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.print("Hum: ");
  display.setTextSize(2);
  display.setCursor(42, 32);
  snprintf(buf, sizeof(buf), "%.1f", g_humidity);
  display.print(buf);
  display.setTextSize(1);
  display.print(" %");

  // ── Light + Battery ────────────────────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 54);
  snprintf(buf, sizeof(buf), "Lux:%.0f", g_light);
  display.print(buf);

  if (g_battery >= 0) {
    snprintf(buf, sizeof(buf), " Bat:%d%%", g_battery);
    display.print(buf);
  }

  // Sensor connection state indicator (small dot in bottom-right)
  bool connected = (g_sensorState == "CONNECTED");
  if (connected) {
    display.fillCircle(SCREEN_WIDTH - 4, 57, 3, SSD1306_WHITE);  // solid = connected
  } else {
    display.drawCircle(SCREEN_WIDTH - 4, 57, 3, SSD1306_WHITE);  // hollow = disconnected
  }

  display.display();
}

// ───────────────────────────────────────────────────────────────────────────────
// Helper: two-line status screen for startup / transitions
// ───────────────────────────────────────────────────────────────────────────────
void showStatus(const String& line1, const String& line2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(line1);
  if (line2.length() > 0) {
    display.setCursor(0, 36);
    display.println(line2);
  }
  display.display();
}
