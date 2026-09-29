#include <Arduino.h>
#include <WiFi.h>
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
const char* SERVER_URL = "http://10.165.255.7:3000/data";

// ==========================================
// 3. Sensor & Cylinder Parameters
// ==========================================
const int SENSOR_PIN = 34;
const float VOLTAGE_DIVIDER_RATIO = 1.50;
const float SENSOR_MIN_VOLTAGE    = 0.50;
const float SENSOR_MAX_VOLTAGE    = 4.50;
const float SENSOR_MAX_BAR        = 250.0;
const float FULL_CYLINDER_BAR     = 200.0;
const float EMPTY_CYLINDER_BAR    = 10.0;
const float CYLINDER_WATER_VOL_L  = 10.0;

// Moving average smoothing
const int NUM_READINGS = 20;
float readings[NUM_READINGS];
int readIndex = 0;
float total = 0.0;

unsigned long lastUpdateMs = 0;
const unsigned long UPDATE_INTERVAL_MS = 1000;

// Previous values for flow rate and estimation tracking
float prevBar = -1.0;
float prevPercent = -1.0;
unsigned long previousTimeMs = 0;
float estimatedFlowRateLmin = 0.0;

// ==========================================
// 4. Helper Functions
// ==========================================
float getSmoothedTransducerVoltage() {
  uint32_t rawMv = analogReadMilliVolts(SENSOR_PIN);
  float espVoltage = rawMv / 1000.0;
  float actualVoltage = espVoltage * VOLTAGE_DIVIDER_RATIO;

  total = total - readings[readIndex];
  readings[readIndex] = actualVoltage;
  total = total + readings[readIndex];
  readIndex = (readIndex + 1) % NUM_READINGS;

  return (total / NUM_READINGS);
}

float voltageToBar(float voltage) {
  if (voltage <= SENSOR_MIN_VOLTAGE) return 0.0;
  if (voltage >= SENSOR_MAX_VOLTAGE) return SENSOR_MAX_BAR;
  return ((voltage - SENSOR_MIN_VOLTAGE) / (SENSOR_MAX_VOLTAGE - SENSOR_MIN_VOLTAGE)) * SENSOR_MAX_BAR;
}

float barToPercentage(float bar) {
  if (bar <= EMPTY_CYLINDER_BAR) return 0.0;
  if (bar >= FULL_CYLINDER_BAR) return 100.0;
  return ((bar - EMPTY_CYLINDER_BAR) / (FULL_CYLINDER_BAR - EMPTY_CYLINDER_BAR)) * 100.0;
}

float percentageToBar(float percent) {
  if (percent <= 0.0) return EMPTY_CYLINDER_BAR;
  if (percent >= 100.0) return FULL_CYLINDER_BAR;
  return EMPTY_CYLINDER_BAR + (percent / 100.0) * (FULL_CYLINDER_BAR - EMPTY_CYLINDER_BAR);
}

float getTimeToTargetPercent(float currentBar, float targetPercent, float flowRateLmin) {
  float targetBar = percentageToBar(targetPercent);
  if (currentBar <= targetBar) return 0.0;
  
  float currentLiters = CYLINDER_WATER_VOL_L * currentBar;
  float targetLiters  = CYLINDER_WATER_VOL_L * targetBar;
  float volumeToConsume = currentLiters - targetLiters;
  
  float activeFlowRate = (flowRateLmin > 0.1) ? flowRateLmin : 5.0;
  return volumeToConsume / activeFlowRate;
}

// ==========================================
// 5. TFT UI Drawing Functions (ST7789 True 320x240 Landscape)
// ==========================================
void drawStaticDashboardUI() {
  tft.fillScreen(ILI9341_BLACK);

  // 1. Top Header Banner (320 x 28)
  tft.fillRect(0, 0, 320, 28, ILI9341_NAVY);
  tft.drawFastHLine(0, 28, 320, ILI9341_CYAN);

  tft.setCursor(8, 6);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.print("OXYGEN MONITOR");

  // 2. Top-Left Box: Oxygen Level Hero (Width: 152, Height: 98)
  tft.drawRoundRect(4, 32, 152, 98, 6, ILI9341_DARKGREY);
  tft.setCursor(10, 38);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("OXYGEN LEVEL");

  // 3. Top-Right Box: Pressure BAR (Width: 156, Height: 98)
  tft.drawRoundRect(160, 32, 156, 98, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 38);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("PRESSURE (BAR)");

  // 4. Bottom-Left Box: Remaining Volume (Width: 152, Height: 102)
  tft.drawRoundRect(4, 134, 152, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(10, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("REMAINING VOL");

  // 5. Bottom-Right Box: Time to Empty (Width: 156, Height: 102)
  tft.drawRoundRect(160, 134, 156, 102, 6, ILI9341_DARKGREY);
  tft.setCursor(166, 140);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("TIME TO EMPTY");
}

void updateDisplayData(float currentBar, float percent, float liters, float timeRemainingMins, String statusStr) {
  uint16_t percentColor = ILI9341_GREEN;
  if (percent <= 15.0) percentColor = ILI9341_RED;
  else if (percent <= 30.0) percentColor = ILI9341_YELLOW;

  // --- 1. Top-Left Box: Oxygen Percentage ---
  tft.fillRect(90, 36, 62, 12, ILI9341_BLACK);
  tft.setCursor(92, 38);
  tft.setTextColor(percentColor);
  tft.setTextSize(1);
  tft.print(statusStr);

  tft.fillRect(10, 52, 140, 36, ILI9341_BLACK);
  tft.setCursor(10, 54);
  tft.setTextColor(percentColor);
  tft.setTextSize(3);
  tft.printf("%.1f", percent);
  tft.setTextSize(2);
  tft.print("%");

  tft.drawRect(10, 94, 140, 12, ILI9341_DARKGREY);
  tft.fillRect(11, 95, 138, 10, ILI9341_BLACK);
  int barW = (int)((percent / 100.0) * 138.0);
  if (barW > 138) barW = 138;
  if (barW > 0) tft.fillRect(11, 95, barW, 10, percentColor);

  // --- 2. Top-Right Box: Pressure (BAR) ---
  tft.fillRect(166, 54, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 56);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(3);
  tft.printf("%.1f", currentBar);
  tft.setTextSize(1);
  tft.print(" BAR");

  tft.setCursor(166, 104);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("(0-250 BAR Max)");

  // --- 3. Bottom-Left Box: Volume (Liters) ---
  tft.fillRect(10, 156, 140, 40, ILI9341_BLACK);
  tft.setCursor(10, 158);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(3);
  tft.printf("%.0f", liters);
  tft.setTextSize(1);
  tft.print(" L");

  tft.setCursor(10, 206);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.print("10L Water Vol");

  // --- 4. Bottom-Right Box: Time to Empty (Mins) ---
  tft.fillRect(166, 156, 144, 40, ILI9341_BLACK);
  tft.setCursor(166, 158);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(3);
  tft.printf("%.0f", timeRemainingMins);
  tft.setTextSize(1);
  tft.print(" Mins");

  int hrs = (int)(timeRemainingMins / 60);
  int remM = (int)timeRemainingMins % 60;
  tft.fillRect(166, 204, 144, 14, ILI9341_BLACK);
  tft.setCursor(166, 206);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.printf("~%dh %dm left", hrs, remM);
}

void updateWifiStatusUI(bool connected, String ipStr) {
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
  Serial.println("\n--- Initializing ESP32 Oxygen Monitor & ST7789 TFT Display ---");

  // Initialize ADC
  analogReadResolution(12);
  pinMode(SENSOR_PIN, INPUT);
  for (int i = 0; i < NUM_READINGS; i++) readings[i] = 0.0;

  // Initialize ST7789 TFT Display in Landscape Mode (320x240)
  tft.init(240, 320); // Initialize ST7789 panel resolution
  tft.setRotation(1); // Set True Landscape mode (320x240)
  tft.invertDisplay(false); // Disable hardware color inversion for deep dark background
  drawStaticDashboardUI();

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID);
  
  Serial.print("Connecting to Wi-Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    updateWifiStatusUI(true, WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection pending...");
    updateWifiStatusUI(false, "");
  }
}

void loop() {
  unsigned long currentMs = millis();
  if (currentMs - lastUpdateMs >= UPDATE_INTERVAL_MS) {
    lastUpdateMs = currentMs;

    // 1. Read Transducer Data
    float voltage = getSmoothedTransducerVoltage();
    float currentBar = voltageToBar(voltage);

    // If transducer isn't active/connected yet, generate test demonstration values
    if (voltage < 0.2) {
      currentBar = 185.0 + (random(-30, 30) / 10.0); // Demo ~185 BAR
    }

    float oxygenPercent = barToPercentage(currentBar);
    float remainingLiters = CYLINDER_WATER_VOL_L * currentBar;

    // 2. Flow Rate & Time Remaining Calculation
    if (prevBar > 0.0 && currentMs > previousTimeMs) {
      float timeDiffMinutes = (currentMs - previousTimeMs) / 60000.0;
      float barDrop = prevBar - currentBar;
      
      if (barDrop > 0.1 && timeDiffMinutes > 0) {
        float instantaneousFlowRate = (barDrop * CYLINDER_WATER_VOL_L) / timeDiffMinutes;
        estimatedFlowRateLmin = (estimatedFlowRateLmin * 0.8) + (instantaneousFlowRate * 0.2);
      }
    }
    prevBar = currentBar;
    previousTimeMs = currentMs;

    float timeRemainingMins = 0;
    if (estimatedFlowRateLmin > 0.1) {
      timeRemainingMins = remainingLiters / estimatedFlowRateLmin;
    } else {
      timeRemainingMins = remainingLiters / 5.0; // Baseline 5 L/min flow assumption
    }

    float timeToTarget20Mins = getTimeToTargetPercent(currentBar, 20.0, estimatedFlowRateLmin);

    String statusStr = "NORMAL";
    if (oxygenPercent <= 15.0) statusStr = "CRITICAL";
    else if (oxygenPercent <= 30.0) statusStr = "LOW";

    // 3. Update ST7789 Display Screen
    updateDisplayData(currentBar, oxygenPercent, remainingLiters, timeRemainingMins, statusStr);

    // 4. Send HTTP POST to Node.js Dashboard Server
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(SERVER_URL);
      http.addHeader("Content-Type", "application/json");

      char body[256];
      snprintf(body, sizeof(body),
        "{\"pressure_bar\":%.1f,\"oxygen_percent\":%.1f,\"remaining_liters\":%.0f,\"flow_rate_lmin\":%.1f,\"time_remaining_mins\":%.0f,\"time_to_target_20_mins\":%.0f,\"status\":\"%s\"}",
        currentBar, oxygenPercent, remainingLiters, estimatedFlowRateLmin, timeRemainingMins, timeToTarget20Mins, statusStr.c_str());

      int httpCode = http.POST(body);
      Serial.printf("POST Telemetry -> Code: %d\n", httpCode);
      http.end();
    } else {
      // Retry Wi-Fi connection periodically if disconnected
      WiFi.begin(WIFI_SSID);
    }
  }
}
