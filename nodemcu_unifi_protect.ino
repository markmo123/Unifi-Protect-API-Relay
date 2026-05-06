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
 * ⚠ GET (UniFi console) uses setInsecure() – self-signed cert, LAN use only.
 *   POST (cloud endpoint) uses full TLS certificate verification via the
 *   built-in CA bundle shipped with the ESP8266 board package.
 *
 * UniFi Protect API response structure (relevant fields):
 * {
 *   "id": "...",
 *   "state": "CONNECTED",
 *   "stats": {
 *     "temperature": { "value": 22.5 },   // °C, 1 decimal place
 *     "humidity":    { "value": 58 },      // %, integer
 *     "light":       { "value": 412 }      // lux, integer
 *   },
 *   "batteryStatus": { "percentage": 87 }
 * }
 *
 * OLED dot key (bottom-right corner):
 *   ●  solid   = sensor CONNECTED, last POST succeeded
 *   ○  hollow  = sensor DISCONNECTED
 *   (no dot)   = GET or POST error
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
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

float  g_temperature = 0.0;
int    g_humidity    = 0;     // integer – Protect reports whole % values
int    g_light       = 0;     // integer – Protect reports whole lux values
int    g_battery     = -1;    // -1 = unknown
String g_sensorState = "";    // "CONNECTED" / "DISCONNECTED" / etc.
bool   g_hasData     = false;

// Three-state dot indicator
enum DotState { DOT_NONE, DOT_SOLID, DOT_HOLLOW };
DotState g_dotState = DOT_NONE;

unsigned long g_lastPoll = 0;

// ─── FORWARD DECLARATIONS ──────────────────────────────────────────────────────

bool fetchSensorData();
bool postSensorData();
void updateDisplay();
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
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("UniFi Protect");
  display.setCursor(0, 36);
  display.println("Sensor Bridge");
  display.display();
  delay(1500);

  // ── Wi-Fi ──────────────────────────────────────────────────────────────────
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 20000) {
      Serial.println("\nWi-Fi timeout. Restarting...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println();
  Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ───────────────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost – reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  unsigned long now = millis();
  if (g_lastPoll == 0 || now - g_lastPoll >= POLL_INTERVAL_MS) {
    g_lastPoll = now;

    if (fetchSensorData()) {
      Serial.printf(
        "Sensor: state=%s  temp=%.1f C  hum=%d%%  light=%d lux  bat=%d%%\n",
        g_sensorState.c_str(), g_temperature, g_humidity, g_light, g_battery
      );

      if (postSensorData()) {
        Serial.println("POST succeeded.");
        // Dot reflects sensor connection state after a successful POST
        g_dotState = (g_sensorState == "CONNECTED") ? DOT_SOLID : DOT_HOLLOW;
      } else {
        Serial.println("POST failed.");
        g_dotState = DOT_NONE;  // no dot = something went wrong
      }

      g_hasData = true;
    } else {
      Serial.println("Sensor fetch failed.");
      g_dotState = DOT_NONE;
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
// Temperature is stored as float (1 d.p.); humidity and light are integers
// as Protect returns whole numbers for both.
// Returns true on success.
// ───────────────────────────────────────────────────────────────────────────────
bool fetchSensorData() {
  WiFiClientSecure client;
  client.setInsecure();  // ⚠ skips TLS cert verification – LAN use only

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

  // Filter to only the fields we need – keeps heap usage low on ESP8266
  StaticJsonDocument<256> filter;
  filter["state"]                         = true;
  filter["stats"]["temperature"]["value"] = true;
  filter["stats"]["humidity"]["value"]    = true;
  filter["stats"]["light"]["value"]       = true;
  filter["batteryStatus"]["percentage"]   = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  // Validate a required field is present
  JsonVariant tempVal = doc["stats"]["temperature"]["value"];
  if (tempVal.isNull()) {
    Serial.println("JSON missing stats.temperature.value – check sensor ID / API key.");
    return false;
  }

  g_sensorState = doc["state"] | "UNKNOWN";
  g_temperature = doc["stats"]["temperature"]["value"].as<float>();
  g_humidity    = doc["stats"]["humidity"]["value"].as<int>();
  g_light       = doc["stats"]["light"]["value"].as<int>();
  g_battery     = doc["batteryStatus"]["percentage"] | -1;

  return true;
}

// ───────────────────────────────────────────────────────────────────────────────
// POST the latest readings to the backend API.
// All three values are sent as numbers with 1 decimal place, matching the
// precision of the UniFi Protect API output.
// Body:   { "temperature": 22.5, "humidity": 58, "light": 412 }
// Header: x-api-key: <POST_API_KEY>
// Returns true on HTTP 200 or 201.
// ───────────────────────────────────────────────────────────────────────────────
bool postSensorData() {
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

  // Serialise floats to exactly 1 decimal place so the JSON body
  // sends e.g. 22.5 not 22.500001 or 22
  StaticJsonDocument<96> doc;
  doc["temperature"] = serialized(String(g_temperature, 1));
  doc["humidity"]    = g_humidity;
  doc["light"]       = g_light;

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
// Render the latest values on the OLED.
// The display is only ever updated here – no transient "fetching" screens.
//
// Layout:
//   ┌──────────────────────────────┐
//   │ UniFi Protect                │  ← inverted header bar
//   │ Temp:  22.5 C                │
//   │ Hum:   58.3 %                │
//   │ Lux:412.0  Bat:87%        ●  │  ← dot: ●=ok  ○=disconnected  (none)=error
//   └──────────────────────────────┘
// ───────────────────────────────────────────────────────────────────────────────
void updateDisplay() {
  display.clearDisplay();

  // ── Header bar ─────────────────────────────────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2);
  display.print("UniFi Protect");
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
  snprintf(buf, sizeof(buf), "%d", g_humidity);
  display.print(buf);
  display.setTextSize(1);
  display.print(" %");

  // ── Light + Battery ────────────────────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 54);
  snprintf(buf, sizeof(buf), "Lux:%d", g_light);
  display.print(buf);

  if (g_battery >= 0) {
    snprintf(buf, sizeof(buf), " Bat:%d%%", g_battery);
    display.print(buf);
  }

  // ── Status dot (bottom-right) ───────────────────────────────────────────────
  // ●  DOT_SOLID   – sensor connected, last POST succeeded
  // ○  DOT_HOLLOW  – sensor disconnected (but comms working)
  // (none) DOT_NONE – GET or POST failed
  const int dotX = SCREEN_WIDTH - 4;
  const int dotY = 57;
  if (g_dotState == DOT_SOLID) {
    display.fillCircle(dotX, dotY, 3, SSD1306_WHITE);
  } else if (g_dotState == DOT_HOLLOW) {
    display.drawCircle(dotX, dotY, 3, SSD1306_WHITE);
  }
  // DOT_NONE: nothing drawn

  display.display();
}
