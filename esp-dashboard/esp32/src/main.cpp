#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
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
struct WifiNetworkConfig {
  const char* ssid;
  const char* pass;
};

// Auto-Connect Networks: Primary is laptop network (DMIHER), secondary is hotspot (Rushikesh)
const WifiNetworkConfig KNOWN_NETWORKS[] = {
  { "DMIHER",     "Jio@1234" },
  { "Rushikesh",  "" },
  { "Rushikesh ", "" }
};
const int NUM_KNOWN_NETWORKS = sizeof(KNOWN_NETWORKS) / sizeof(KNOWN_NETWORKS[0]);

// High-speed MQTT Broker (Sub-20ms latency, zero rate-limiting)
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "oura_rushi_9921/oxygen/telemetry";

WiFiClient espMqttClient;
PubSubClient mqttClient(espMqttClient);

volatile bool wifiIsConnected = false;
volatile bool mqttIsConnected = false;

// ==========================================
// 3. Bluetooth Low Energy (Web Bluetooth NUS)
// ==========================================
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define BLE_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pBleServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
volatile bool bleDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      bleDeviceConnected = true;
      Serial.println("[BLE] Web Bluetooth client connected!");
    }

    void onDisconnect(BLEServer* pServer) {
      bleDeviceConnected = false;
      Serial.println("[BLE] Web Bluetooth client disconnected! Restarting advertising...");
      pServer->getAdvertising()->start();
    }
};

void initBLE() {
  BLEDevice::init("Oura Oxygen Monitor");
  pBleServer = BLEDevice::createServer();
  pBleServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pBleServer->createService(BLE_SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        BLE_CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising started as 'Oura Oxygen Monitor'");
}

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
    long calculatedZero = sum / validSamples;
    // Sanity check: Atmospheric room air is between 1.5M and 4.5M counts.
    // If user turned on device while syringe was compressed with air, do not lock high pressure as zero!
    if (calculatedZero >= 1500000 && calculatedZero <= 4500000) {
      zeroOffset = calculatedZero;
      Serial.printf("[Sensor] SUCCESS! Tare Zero Baseline: %ld (from %d samples)\n", zeroOffset, validSamples);
    } else {
      zeroOffset = 2730000; // Calibrated atmospheric baseline
      Serial.printf("[Sensor] Notice: Sensor was pressurized at boot (%ld). Using calibrated baseline: %ld\n", calculatedZero, zeroOffset);
    }
    sensorDetected = true;
  } else {
    zeroOffset = 2730000; // Calibrated ambient baseline
    sensorDetected = true;
    Serial.printf("[Sensor] Using default ambient baseline: %ld\n", zeroOffset);
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

void updateCloudStatusUI(bool wifiOk, bool mqttOk, bool bleOk) {
  tft.fillRect(205, 4, 110, 20, ILI9341_NAVY);
  tft.setCursor(210, 9);
  tft.setTextSize(1);
  if (bleOk) {
    tft.setTextColor(ILI9341_CYAN);
    tft.print("BLE: LIVE \x10");
  } else if (!wifiOk) {
    tft.setTextColor(ILI9341_RED);
    tft.print("WiFi: OFF");
  } else if (!mqttOk) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.print("MQTT: CON...");
  } else {
    tft.setTextColor(ILI9341_GREEN);
    tft.print("SYNC: LIVE");
  }
}

// ==========================================
// 6. FreeRTOS Background Telemetry Task (Runs on Core 0)
// ==========================================
void telemetryTask(void *pvParameters) {
  unsigned long lastSend = 0;
  float lastSentPct = -999.0;
  unsigned long lastWifiAttempt = 0;
  unsigned long lastMqttAttempt = 0;

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(5); // 5 seconds robust timeout for cellular hotspot latency
  mqttClient.setKeepAlive(30);

  // Clean fixed client ID based on hardware MAC
  String macStr = WiFi.macAddress();
  macStr.replace(":", "");
  String clientId = "OuraESP32_" + macStr;

  for (;;) {
    unsigned long now = millis();

    // 1. Manage Wi-Fi connection in the background
    if (WiFi.status() != WL_CONNECTED) {
      wifiIsConnected = false;
      mqttIsConnected = false;

      if (lastWifiAttempt == 0 || (now - lastWifiAttempt >= 5000)) {
        lastWifiAttempt = now;
        static int netIndex = 0;
        const WifiNetworkConfig& net = KNOWN_NETWORKS[netIndex % NUM_KNOWN_NETWORKS];
        netIndex++;
        Serial.printf("[WiFi] Attempting connection to '%s'...\n", net.ssid);
        WiFi.begin(net.ssid, net.pass);
      }
    } else {
      if (!wifiIsConnected) {
        wifiIsConnected = true;
        Serial.println("\n[WiFi] Connected successfully to: " + WiFi.SSID());
        Serial.println("[WiFi] Local IP: " + WiFi.localIP().toString());
      }
    }

    // 2. Manage High-Speed MQTT Connection (non-blocking retry)
    if (wifiIsConnected) {
      if (!mqttClient.connected()) {
        mqttIsConnected = false;
        if (now - lastMqttAttempt >= 2500) {
          lastMqttAttempt = now;
          Serial.println("[MQTT] Connecting to broker.hivemq.com:1883 as " + clientId + "...");
          if (mqttClient.connect(clientId.c_str())) {
            mqttIsConnected = true;
            Serial.println("[MQTT] Connected to broker.hivemq.com:1883 successfully!");
          } else {
            Serial.printf("[MQTT] Connection failed (rc=%d), will retry in 2.5s...\n", mqttClient.state());
          }
        }
      } else {
        mqttIsConnected = true;
        mqttClient.loop();
      }
    } else {
      mqttIsConnected = false;
    }

    // 3. Read live sensor snapshot from Core 1
    float tBar, tKpa, tPct;
    char tStatus[16];
    portENTER_CRITICAL(&stateMux);
    tBar = livePressureBar;
    tKpa = livePressureKpa;
    tPct = livePercent;
    strncpy(tStatus, liveStatusBuf, sizeof(tStatus));
    portEXIT_CRITICAL(&stateMux);

    // Adaptive Event Trigger: Send immediately when user presses, blows, or releases
    bool valueChanged = (fabs(tPct - lastSentPct) >= 0.1) || (tPct == 0.0 && lastSentPct != 0.0) || (tPct > 0.0 && lastSentPct == 0.0);
    bool shouldSend = false;

    // Check if either Bluetooth or Wi-Fi MQTT is ready to transmit
    bool canTransmit = bleDeviceConnected || (wifiIsConnected && mqttClient.connected());

    if (canTransmit) {
      // 50ms fast dispatch when changing: ultra smooth continuous flow (~20 updates/sec)!
      if (valueChanged && (now - lastSend >= 50)) {
        shouldSend = true;
      } else if (now - lastSend >= 500) {
        shouldSend = true; // Steady-state heartbeat
      }
    }

    // 4. Publish telemetry packet (Instant Bluetooth Direct + MQTT Cloud!)
    if (shouldSend) {
      lastSend = now;
      lastSentPct = tPct;

      float simulatedTankBar = (tPct / 100.0) * 200.0;
      float remainingLiters = simulatedTankBar * 10.0;

      char body[256];
      snprintf(body, sizeof(body),
        "{\"pressure_bar\":%.3f,\"pressure_kpa\":%.1f,\"oxygen_percent\":%.1f,\"remaining_liters\":%.0f,\"status\":\"%s\"}",
        tBar, tKpa, tPct, remainingLiters, tStatus);

      // Instant Bluetooth Low Energy Dispatch (Sub-15ms direct wireless to browser!)
      if (bleDeviceConnected && pTxCharacteristic != NULL) {
        pTxCharacteristic->setValue((uint8_t*)body, strlen(body));
        pTxCharacteristic->notify();
      }

      // Cloud MQTT Dispatch (if Wi-Fi online)
      if (wifiIsConnected && mqttClient.connected()) {
        mqttClient.publish(MQTT_TOPIC, body);
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
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
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  updateCloudStatusUI(false, false, false);

  // Initialize Bluetooth Low Energy (Web Bluetooth)
  initBLE();

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
  static bool lastMqttState = false;
  static float smoothedKpa = 0.0;
  static float displayedKpa = -1.0;

  // 4-point rolling average buffer to cancel electronic noise
  static long rawBuf[4] = {0, 0, 0, 0};
  static int rawBufIdx = 0;
  static bool rawBufFilled = false;

  unsigned long currentMs = millis();

  // Read sensor every 80ms (clean sync to HX710B data ready)
  if (currentMs - lastSensorRead >= 80) {
    lastSensorRead = currentMs;

    long raw = readHX710BWithTimeout(120);
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

  // Update Cloud Sync indicator on screen header (WiFi, MQTT & BLE state)
  static bool lastBleState = false;
  if (wifiIsConnected != lastWifiState || mqttIsConnected != lastMqttState || bleDeviceConnected != lastBleState) {
    lastWifiState = wifiIsConnected;
    lastMqttState = mqttIsConnected;
    lastBleState = bleDeviceConnected;
    updateCloudStatusUI(lastWifiState, lastMqttState, lastBleState);
  }

  delay(4);
}
