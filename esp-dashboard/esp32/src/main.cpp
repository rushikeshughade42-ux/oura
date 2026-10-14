#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// ==========================================
// 1. TFT LCD Pin Definitions (VSPI Pins for ST7789)
// ==========================================
#define TFT_CS    5   // Chip Select -> GPIO 5
#define TFT_DC    2   // Data / Command -> GPIO 2
#define TFT_RST   4   // Reset -> GPIO 4
// MOSI -> GPIO 23, SCK -> GPIO 18 (Hardware VSPI)

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Color Definitions (RGB565 16-bit)
#define ILI9341_BLACK     0x0000
#define ILI9341_NAVY      0x000F
#define ILI9341_CYAN      0x07FF
#define ILI9341_WHITE     0xFFFF
#define ILI9341_DARKGREY  0x39E7
#define ILI9341_LIGHTGREY 0x7BEF
#define ILI9341_GREEN     0x07E0
#define ILI9341_RED       0xF800
#define ILI9341_YELLOW    0xFFE0

// ==========================================
// 2. Wi-Fi & Network Configuration
// ==========================================
const char* WIFI_SSID  = "Rushikesh";
const char* WIFI_PASS  = "";

// Use either your local laptop IP: "http://10.20.235.7:3000/data"
// OR your live Vercel URL: "https://your-vercel-domain.vercel.app/data"
const char* SERVER_URL = "http://10.20.235.7:3000/data";

unsigned long lastWifiRetryMs = 0;
bool wifiWasConnected = false;

// ==========================================
// 3. Digital Air Pressure Sensor (MPS20N0040D + HX710B)
// ==========================================
#define SENSOR_OUT_PIN  26  // Data Pin (OUT) -> GPIO 26
#define SENSOR_SCK_PIN  25  // Clock Pin (SCK) -> GPIO 25

// Sensor Specifications: 0 to 40 kPa (0 to 0.40 BAR)
const float MAX_SENSOR_KPA = 40.0;
const float MAX_SENSOR_BAR = 0.40;
const float COUNTS_PER_KPA = 75000.0; // Calibration scale factor for HX710B 24-bit ADC

long zeroOffset = 0;
bool sensorDetected = false;

unsigned long lastUpdateMs = 0;
const unsigned long UPDATE_INTERVAL_MS = 150; // Fast 150ms screen refresh for instant breath response

unsigned long lastHttpMs = 0;
const unsigned long HTTP_INTERVAL_MS = 1000; // Send telemetry to web server every 1 second

// ==========================================
// 4. HX710B Digital Reading Driver
// ==========================================
long readHX710BRaw() {
  // Wait until OUT pin goes LOW (data ready) with 80ms timeout
  unsigned long start = millis();
  while (digitalRead(SENSOR_OUT_PIN) == HIGH) {
    if (millis() - start > 80) {
      return -1; // Timeout / sensor busy
    }
  }

  long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(SENSOR_SCK_PIN, HIGH);
    delayMicroseconds(1);
    count = (count << 1) | digitalRead(SENSOR_OUT_PIN);
    digitalWrite(SENSOR_SCK_PIN, LOW);
    delayMicroseconds(1);
  }

  // 25th clock pulse: Sets differential input mode, Gain 128 for next reading
  digitalWrite(SENSOR_SCK_PIN, HIGH);
  delayMicroseconds(1);
  digitalWrite(SENSOR_SCK_PIN, LOW);
  delayMicroseconds(1);

  // Sign extension for 24-bit two's complement
  if (count & 0x800000) {
    count |= 0xFF000000;
  }
  return count;
}

void tareSensor() {
  long sum = 0;
  int validSamples = 0;
  for (int i = 0; i < 15; i++) {
    long val = readHX710BRaw();
    if (val != -1) {
      sum += val;
      validSamples++;
    }
    delay(20);
  }

  if (validSamples > 0) {
    zeroOffset = sum / validSamples;
    sensorDetected = true;
    Serial.printf("Sensor Calibrated! Zero Offset: %ld (samples: %d)\n", zeroOffset, validSamples);
  } else {
    zeroOffset = 0;
    sensorDetected = false;
    Serial.println("Warning: Sensor not detected on GPIO 26 & 25!");
  }
}

// ==========================================
// 5. TFT UI Drawing Functions (320x240 ST7789)
// ==========================================
void drawStaticDashboardUI() {
  tft.fillScreen(ILI9341_BLACK);

  // 1. Top Header Banner (320 x 28)
  tft.fillRect(0, 0, 320, 28, ILI9341_NAVY);
  tft.drawFastHLine(0, 28, 320, ILI9341_CYAN);

  tft.setCursor(8, 6);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.print("AIR PRESSURE MONITOR");

  // 2. Top-Left Box: Air Pressure % (Width: 152, Height: 98)
  tft.drawRoundRect(4, 32, 152, 98, 6, ILI9341_DARKGREY);
  tft.setCursor(10, 38);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE LEVEL");

  // 3. Top-Right Box: Pressure BAR (Width: 156, Height: 98)
  tft.drawRoundRect(160, 32, 156, 98, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 38);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE (BAR)");

  // 4. Bottom-Left Box: Pressure in kPa (Width: 152, Height: 102)
  tft.drawRoundRect(4, 134, 152, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(10, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE (kPa)");

  // 5. Bottom-Right Box: Sensor Status (Width: 156, Height: 102)
  tft.drawRoundRect(160, 134, 156, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("AIR FLOW STATUS");
}

void updateDisplayData(float currentBar, float currentKpa, float percent, String statusStr) {
  uint16_t color = ILI9341_GREEN;
  if (percent > 60.0) color = ILI9341_YELLOW;
  if (percent > 85.0) color = ILI9341_RED;

  // --- 1. Top-Left Box: Pressure % & Progress Bar ---
  tft.fillRect(95, 36, 58, 12, ILI9341_BLACK);
  tft.setCursor(95, 38);
  tft.setTextColor(color);
  tft.setTextSize(1);
  tft.print(statusStr);

  tft.fillRect(10, 52, 140, 36, ILI9341_BLACK);
  tft.setCursor(10, 54);
  tft.setTextColor(color);
  tft.setTextSize(3);
  tft.printf("%.1f", percent);
  tft.setTextSize(2);
  tft.print("%");

  // Progress Bar
  tft.drawRect(10, 94, 140, 12, ILI9341_DARKGREY);
  tft.fillRect(11, 95, 138, 10, ILI9341_BLACK);
  int barW = (int)((percent / 100.0) * 138.0);
  if (barW > 138) barW = 138;
  if (barW > 0) tft.fillRect(11, 95, barW, 10, color);

  // --- 2. Top-Right Box: Pressure (BAR with 3 Decimals) ---
  tft.fillRect(166, 54, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 56);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(3);
  tft.printf("%.3f", currentBar);
  tft.setTextSize(1);
  tft.print(" BAR");

  tft.fillRect(166, 104, 144, 12, ILI9341_BLACK);
  tft.setCursor(166, 104);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("Max: 0.400 BAR");

  // --- 3. Bottom-Left Box: Pressure in kPa ---
  tft.fillRect(10, 156, 140, 40, ILI9341_BLACK);
  tft.setCursor(10, 158);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(3);
  tft.printf("%.1f", currentKpa);
  tft.setTextSize(1);
  tft.print(" kPa");

  tft.fillRect(10, 206, 140, 12, ILI9341_BLACK);
  tft.setCursor(10, 206);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("Range: 0-40 kPa");

  // --- 4. Bottom-Right Box: Live Status ---
  tft.fillRect(166, 156, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 160);
  if (percent > 3.0) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(2);
    tft.print("FLOWING");
  } else {
    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.print("IDLE");
  }

  tft.fillRect(166, 204, 144, 14, ILI9341_BLACK);
  tft.setCursor(166, 206);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  if (sensorDetected) {
    tft.print("Sensor: READY");
  } else {
    tft.setTextColor(ILI9341_RED);
    tft.print("Check Pins G26/25");
  }
}

void updateWifiStatusUI(bool connected) {
  tft.fillRect(230, 4, 85, 20, ILI9341_NAVY);
  tft.setCursor(235, 9);
  tft.setTextSize(1);
  if (connected) {
    tft.setTextColor(ILI9341_GREEN);
    tft.print("WiFi: ON");
  } else {
    tft.setTextColor(ILI9341_RED);
    tft.print("WiFi: OFF");
  }
}

// ==========================================
// 6. Setup & Main Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- Initializing ESP32 Air Pressure Monitor & ST7789 TFT Display ---");

  // Configure Digital Sensor Pins
  pinMode(SENSOR_SCK_PIN, OUTPUT);
  pinMode(SENSOR_OUT_PIN, INPUT);
  digitalWrite(SENSOR_SCK_PIN, LOW);

  // Initialize ST7789 TFT Display in Landscape Mode (320x240)
  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  drawStaticDashboardUI();

  // Show "Calibrating..." notice on screen
  tft.setCursor(166, 160);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.print("CALIBRATING");
  delay(200);

  // Auto-Tare the sensor at startup (zero baseline)
  tareSensor();

  // Connect to Wi-Fi non-blocking attempt
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 6) { // Quick 3-sec check
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    updateWifiStatusUI(true);
  } else {
    Serial.println("\nWiFi connection pending (running offline mode)...");
    updateWifiStatusUI(false);
  }
}

void loop() {
  unsigned long currentMs = millis();

  // Fast 150ms screen refresh for immediate blowing / breath response
  if (currentMs - lastUpdateMs >= UPDATE_INTERVAL_MS) {
    lastUpdateMs = currentMs;

    // 1. Read Digital Sensor
    long raw = readHX710BRaw();
    float pressureKpa = 0.0;
    float pressureBar = 0.0;
    float percent = 0.0;
    String statusStr = "IDLE";

    if (raw != -1 && zeroOffset != 0) {
      long diff = raw - zeroOffset;
      if (diff < 0) diff = 0; // Filter out slight ambient negative jitter

      // Convert delta raw counts to physical pressure
      pressureKpa = (float)diff / COUNTS_PER_KPA;
      if (pressureKpa > MAX_SENSOR_KPA) pressureKpa = MAX_SENSOR_KPA;

      pressureBar = pressureKpa / 100.0; // 100 kPa = 1.0 BAR
      percent = (pressureKpa / MAX_SENSOR_KPA) * 100.0;

      if (percent > 3.0) {
        statusStr = "ACTIVE";
      }
    }

    // 2. Refresh Screen
    updateDisplayData(pressureBar, pressureKpa, percent, statusStr);

    // 3. Serial Debug Print
    Serial.printf("Raw: %ld | Delta: %ld | Pressure: %.3f BAR | %.1f kPa | Level: %.1f%%\n",
                  raw, (raw - zeroOffset), pressureBar, pressureKpa, percent);
  }

  // 4. Background Wi-Fi Auto-Reconnection & Status Tracking
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      Serial.println("\n[WiFi] Connection lost. Switching to offline mode.");
      updateWifiStatusUI(false);
    }

    if (currentMs - lastWifiRetryMs >= 6000) {
      lastWifiRetryMs = currentMs;
      Serial.println("[WiFi] Attempting background reconnect...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  } else {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("\n[WiFi] Reconnected! IP: " + WiFi.localIP().toString());
      updateWifiStatusUI(true);
    }
  }

  // 5. Send HTTP/HTTPS Telemetry to Dashboard (every 1 second)
  if (currentMs - lastHttpMs >= HTTP_INTERVAL_MS) {
    lastHttpMs = currentMs;

    if (WiFi.status() == WL_CONNECTED) {
      long raw = readHX710BRaw();
      long diff = (raw != -1 && zeroOffset != 0) ? (raw - zeroOffset) : 0;
      if (diff < 0) diff = 0;
      float kpa = (float)diff / COUNTS_PER_KPA;
      float bar = kpa / 100.0;
      float pct = (kpa / MAX_SENSOR_KPA) * 100.0;
      float simulatedTankBar = (pct / 100.0) * 200.0;
      float remainingLiters = simulatedTankBar * 10.0;

      char body[256];
      snprintf(body, sizeof(body),
        "{\"pressure_bar\":%.3f,\"pressure_kpa\":%.1f,\"oxygen_percent\":%.1f,\"remaining_liters\":%.0f,\"status\":\"%s\"}",
        bar, kpa, pct, remainingLiters, (pct > 3.0 ? "ACTIVE" : "IDLE"));

      String urlStr = String(SERVER_URL);
      HTTPClient http;

      if (urlStr.startsWith("https://")) {
        WiFiClientSecure secureClient;
        secureClient.setInsecure(); // Allow Vercel SSL certificates without root cert bundle
        if (http.begin(secureClient, urlStr)) {
          http.addHeader("Content-Type", "application/json");
          int httpCode = http.POST(body);
          if (httpCode > 0) {
            Serial.printf("POST Vercel -> Code: %d\n", httpCode);
          }
          http.end();
        }
      } else {
        if (http.begin(urlStr)) {
          http.addHeader("Content-Type", "application/json");
          int httpCode = http.POST(body);
          if (httpCode > 0) {
            Serial.printf("POST Local -> Code: %d\n", httpCode);
          }
          http.end();
        }
      }
    }
  }
}
