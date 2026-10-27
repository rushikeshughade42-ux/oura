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

// Real-time Cloud Telemetry Stream (HTTP Port 80 - sub-50ms latency, zero SSL overhead)
const char* STREAM_URL = "http://ntfy.sh/oura_bio_esp32_sensor";

// Live Vercel Production URL for Cloud Telemetry
const char* SERVER_URL = "https://oura-j6w4.vercel.app/data";

volatile bool wifiIsConnected = false;

// ==========================================
// 3. Digital Air Pressure Sensor (MPS20N0040D + HX710B)
// ==========================================
int activeOutPin = 26; // Data Pin (OUT) - GPIO 26
int activeSckPin = 25; // Clock Pin (SCK) - GPIO 25

// Sensor Specifications: 0 to 40 kPa (0 to 0.40 BAR)
const float MAX_SENSOR_KPA = 40.0;
const float COUNTS_PER_KPA = 75000.0; // Calibration scale factor for HX710B 24-bit ADC

long zeroOffset = 3620000; // Baseline room atmospheric pressure (~3.62M counts)
bool sensorDetected = false;
long lastRawReading = 0;

// Global Live Telemetry State (shared with Core 0 telemetry task)
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile float livePressureKpa = 0.0;
volatile float livePressureBar = 0.0;
volatile float livePercent = 0.0;
char liveStatusBuf[16] = "IDLE";

// ==========================================
// 4. HX710B 24-Bit Reading Driver (Synchronized to 10Hz)
// ==========================================
portMUX_TYPE hx710Mux = portMUX_INITIALIZER_UNLOCKED;

long readHX710BWithTimeout(int timeoutMs) {
  digitalWrite(activeSckPin, LOW);
  unsigned long start = millis();

  // Wait until OUT pin goes LOW (data ready)
  while (digitalRead(activeOutPin) == HIGH) {
    if (millis() - start > (unsigned long)timeoutMs) {
      return -1; // Conversion not ready or timeout
    }
    delayMicroseconds(50);
  }

  portENTER_CRITICAL(&hx710Mux);

  long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(activeSckPin, HIGH);
    delayMicroseconds(2);
    count = (count << 1) | digitalRead(activeOutPin);
    digitalWrite(activeSckPin, LOW);
    delayMicroseconds(2);
  }

  // 25th clock pulse: Sets Channel A, Gain 128 for next conversion
  digitalWrite(activeSckPin, HIGH);
  delayMicroseconds(2);
  digitalWrite(activeSckPin, LOW);
  delayMicroseconds(2);

  portEXIT_CRITICAL(&hx710Mux);

  // Sign extension for 24-bit two's complement
  if (count & 0x800000) {
    count |= 0xFF000000;
  }
  return count;
}

// Tare Calibration at startup: Takes clean 10-sample average of room air
void tareSensor() {
  Serial.println("\n[Sensor] Initializing baseline calibration...");
  pinMode(activeSckPin, OUTPUT);
  digitalWrite(activeSckPin, LOW);
  pinMode(activeOutPin, INPUT_PULLUP);

  long sum = 0;
  int validSamples = 0;

  // Settle time for power rail
  delay(300);

  // Take 10 clean samples with 120ms delay (clean 10Hz cycle sync)
  for (int i = 0; i < 10; i++) {
    long val = readHX710BWithTimeout(150);
    if (val > 1000000) {
      sum += val;
      validSamples++;
      Serial.printf("  Tare Sample #%d: %ld\n", validSamples, val);
    }
    delay(120);
  }

  if (validSamples >= 3) {
    zeroOffset = sum / validSamples;
    sensorDetected = true;
    Serial.printf("[Sensor] SUCCESS! Tare Zero Baseline: %ld (from %d samples)\n", zeroOffset, validSamples);
  } else {
    zeroOffset = 3620000; // Expected ambient baseline
    sensorDetected = true;
    Serial.printf("[Sensor] Using ambient baseline: %ld\n", zeroOffset);
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

  // Progress Bar Outline
  tft.drawRect(10, 94, 140, 12, ILI9341_DARKGREY);
  tft.fillRect(11, 95, 138, 10, ILI9341_BLACK);

  // 3. Top-Right Box: Pressure BAR (Width: 156, Height: 98)
  tft.drawRoundRect(160, 32, 156, 98, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 38);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE (BAR)");

  tft.fillRect(166, 104, 144, 12, ILI9341_BLACK);
  tft.setCursor(166, 104);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("Max: 0.400 BAR");

  // 4. Bottom-Left Box: Pressure in kPa (Width: 152, Height: 102)
  tft.drawRoundRect(4, 134, 152, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(10, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE (kPa)");

  tft.fillRect(10, 206, 140, 12, ILI9341_BLACK);
  tft.setCursor(10, 206);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("Range: 0-40 kPa");

  // 5. Bottom-Right Box: Sensor Status (Width: 156, Height: 102)
  tft.drawRoundRect(160, 134, 156, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("AIR FLOW STATUS");
}

void updateDisplayData(float currentBar, float currentKpa, float percent, const char* statusStr) {
  static int lastBarW = 0;
  static uint16_t lastColor = 0xFFFF;

  // Color thresholds: 100-75% Green, 75-25% Yellow, 25-0% Red
  uint16_t color = ILI9341_RED;
  if (percent >= 75.0) {
    color = ILI9341_GREEN;
  } else if (percent >= 25.0) {
    color = ILI9341_YELLOW;
  } else {
    color = ILI9341_RED;
  }

  // --- 1. Top-Left Box: Pressure % ---
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

  // Zero-Flicker Differential Progress Bar Animation
  int barW = (int)((percent / 100.0) * 138.0);
  if (barW > 138) barW = 138;
  if (barW < 0) barW = 0;

  if (color != lastColor) {
    // If color threshold crossed, redraw full bar with new color
    tft.fillRect(11, 95, 138, 10, ILI9341_BLACK);
    if (barW > 0) tft.fillRect(11, 95, barW, 10, color);
    lastColor = color;
  } else {
    // Smooth differential fill (only modifies changed pixels - zero flicker!)
    if (barW > lastBarW) {
      tft.fillRect(11 + lastBarW, 95, barW - lastBarW, 10, color);
    } else if (barW < lastBarW) {
      tft.fillRect(11 + barW, 95, lastBarW - barW, 10, ILI9341_BLACK);
    }
  }
  lastBarW = barW;

  // --- 2. Top-Right Box: Pressure (BAR with 3 Decimals) ---
  tft.fillRect(166, 54, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 56);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(3);
  tft.printf("%.3f", currentBar);
  tft.setTextSize(1);
  tft.print(" BAR");

  // --- 3. Bottom-Left Box: Pressure in kPa ---
  tft.fillRect(10, 156, 140, 40, ILI9341_BLACK);
  tft.setCursor(10, 158);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(3);
  tft.printf("%.1f", currentKpa);
  tft.setTextSize(1);
  tft.print(" kPa");

  // --- 4. Bottom-Right Box: Live Status ---
  tft.fillRect(166, 156, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 160);
  if (percent > 2.0) {
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
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.printf("Pins: G%d/G%d | %ldk", activeOutPin, activeSckPin, lastRawReading / 1000);
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
// 6. FreeRTOS Background Telemetry Task (Runs on Core 0)
// ==========================================
void telemetryTask(void *pvParameters) {
  unsigned long lastSend = 0;
  float lastSentPct = -999.0;
  bool isFirstConnect = true;

  for (;;) {
    // 1. Manage Wi-Fi connection in the background
    if (WiFi.status() != WL_CONNECTED) {
      wifiIsConnected = false;

      // Configure Google DNS (8.8.8.8) to prevent hotspot DNS lookup failures
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));

      WiFi.begin("Rushikesh ", WIFI_PASS);
      unsigned long tryStart = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - tryStart < 2500) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }

      if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin("Rushikesh", WIFI_PASS);
        tryStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - tryStart < 2500) {
          vTaskDelay(100 / portTICK_PERIOD_MS);
        }
      }

      wifiIsConnected = (WiFi.status() == WL_CONNECTED);
      if (wifiIsConnected && isFirstConnect) {
        isFirstConnect = false;
        Serial.println("\n[WiFi] Connected successfully! Local IP: " + WiFi.localIP().toString());
      }
    } else {
      wifiIsConnected = true;
    }

    // 2. Read live sensor snapshot from Core 1
    float tBar, tKpa, tPct;
    char tStatus[16];
    portENTER_CRITICAL(&stateMux);
    tBar = livePressureBar;
    tKpa = livePressureKpa;
    tPct = livePercent;
    strncpy(tStatus, liveStatusBuf, sizeof(tStatus));
    portEXIT_CRITICAL(&stateMux);

    unsigned long now = millis();

    // Adaptive Event Trigger: Send immediately when user presses, blows, or releases
    bool valueChanged = (fabs(tPct - lastSentPct) >= 0.2) || (tPct == 0.0 && lastSentPct != 0.0) || (tPct > 0.0 && lastSentPct == 0.0);
    bool shouldSend = false;

    if (wifiIsConnected) {
      if (valueChanged && (now - lastSend >= 200)) {
        shouldSend = true; // High-frequency fast sync on movement
      } else if (now - lastSend >= 1200) {
        shouldSend = true; // Steady-state heartbeat
      }
    }

    // 3. Post telemetry to high-speed cloud stream
    if (shouldSend) {
      lastSend = now;
      lastSentPct = tPct;

      float simulatedTankBar = (tPct / 100.0) * 200.0;
      float remainingLiters = simulatedTankBar * 10.0;

      char body[256];
      snprintf(body, sizeof(body),
        "{\"pressure_bar\":%.3f,\"pressure_kpa\":%.1f,\"oxygen_percent\":%.1f,\"remaining_liters\":%.0f,\"status\":\"%s\"}",
        tBar, tKpa, tPct, remainingLiters, tStatus);

      WiFiClient client;
      HTTPClient http;
      if (http.begin(client, STREAM_URL)) {
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(800);
        int httpCode = http.POST(body);
        if (httpCode > 0) {
          http.getString(); // Consume response
        }
        http.end();
      }
      client.stop();
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// 7. Setup & Main Loop (Runs on Core 1)
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n--- Initializing ESP32 Air Pressure Monitor & ST7789 TFT Display ---");

  // Configure Digital Sensor Pins
  pinMode(activeSckPin, OUTPUT);
  pinMode(activeOutPin, INPUT_PULLUP);
  digitalWrite(activeSckPin, LOW);

  // Initialize ST7789 TFT Display in Landscape Mode (320x240)
  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false);
  drawStaticDashboardUI();

  // Show "CALIBRATING ZERO..." notice on screen
  tft.setCursor(166, 160);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.print("ZEROING...");

  // Calibrate Zero Baseline at atmospheric room air
  tareSensor();

  // Draw initial clean 0.0 values
  updateDisplayData(0.0, 0.0, 0.0, "IDLE");

  // Start Wi-Fi in background
  WiFi.mode(WIFI_STA);

  // Spawn Background FreeRTOS Telemetry Task on Core 0
  xTaskCreatePinnedToCore(
    telemetryTask,
    "TelemetryTask",
    8192,
    NULL,
    1,
    NULL,
    0 // Core 0
  );
}

void loop() {
  static unsigned long lastSensorRead = 0;
  static bool lastWifiState = false;
  static float smoothedKpa = 0.0;
  static float displayedKpa = -1.0;

  // 4-point rolling average buffer to cancel electronic noise
  static long rawBuf[4] = {0, 0, 0, 0};
  static int rawBufIdx = 0;
  static bool rawBufFilled = false;

  unsigned long currentMs = millis();

  // Read sensor every 110ms (clean 10Hz sync)
  if (currentMs - lastSensorRead >= 110) {
    lastSensorRead = currentMs;

    long raw = readHX710BWithTimeout(130);
    if (raw > 1000000) {
      lastRawReading = raw;

      // Add to rolling window
      rawBuf[rawBufIdx] = raw;
      rawBufIdx = (rawBufIdx + 1) % 4;
      if (rawBufIdx == 0) rawBufFilled = true;

      long avgRaw = raw;
      if (rawBufFilled) {
        avgRaw = (rawBuf[0] + rawBuf[1] + rawBuf[2] + rawBuf[3]) / 4;
      }

      // Calculate gauge pressure above atmospheric baseline
      long diff = avgRaw - zeroOffset;

      // Hysteresis Noise Deadband: If within 25,000 counts (~0.33 kPa or ~0.8%), snap firmly to ZERO
      if (diff < 25000) {
        diff = 0;
      }

      float targetKpa = (float)diff / COUNTS_PER_KPA;
      if (targetKpa > MAX_SENSOR_KPA) targetKpa = MAX_SENSOR_KPA;

      // High-grade Exponential Smoothing (78% previous + 22% target):
      // Buttery smooth transition without any jerky jumping
      if (diff == 0) {
        smoothedKpa = smoothedKpa * 0.70; // Graceful, damped glide to zero
        if (smoothedKpa < 0.10) smoothedKpa = 0.0;
      } else {
        smoothedKpa = smoothedKpa * 0.78 + targetKpa * 0.22;
      }

      float currentBar = smoothedKpa / 100.0;
      float currentPct = (smoothedKpa / MAX_SENSOR_KPA) * 100.0;
      if (currentPct > 100.0) currentPct = 100.0;

      const char* statusStr = (currentPct > 2.0) ? "FLOWING" : "IDLE";

      // Update shared telemetry state for background Core 0 task
      portENTER_CRITICAL(&stateMux);
      livePressureKpa = smoothedKpa;
      livePressureBar = currentBar;
      livePercent = currentPct;
      strncpy(liveStatusBuf, statusStr, sizeof(liveStatusBuf));
      portEXIT_CRITICAL(&stateMux);

      // Smooth display redraw: Only refresh if digit changed by >= 0.1% or snapped to/from zero
      if (fabs(smoothedKpa - displayedKpa) >= 0.1 || (smoothedKpa == 0.0 && displayedKpa != 0.0)) {
        displayedKpa = smoothedKpa;
        updateDisplayData(currentBar, smoothedKpa, currentPct, statusStr);
      }
    }
  }

  // Update Wi-Fi indicator if connection status changed
  if (wifiIsConnected != lastWifiState) {
    lastWifiState = wifiIsConnected;
    updateWifiStatusUI(lastWifiState);
  }

  delay(4);
}
