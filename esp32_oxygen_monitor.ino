/*
  ==================================================================================
  ESP32 High-Pressure Oxygen Cylinder Monitor & WebSocket Live Streamer
  ==================================================================================
  Hardware:
    - ESP32-WROOM-32
    - High Pressure Transducer (0 - 250 BAR / 0.5V - 4.5V Output)
    - Voltage Divider: R1 = 10k ohm, R2 = 20k ohm (Reduces 4.5V max output to safe 3.0V for ESP32)
    
  Pin Connections:
    - Transducer VCC -> 5V
    - Transducer GND -> GND
    - Transducer OUT -> R1 (10k) -> Node -> ESP32 GPIO 34 (ADC1_CH6)
                                   -> R2 (20k) -> GND
  ==================================================================================
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ==========================================
// 1. Wi-Fi & Network Configuration
// ==========================================
const char* WIFI_SSID     = "Rushikesh";      // Wi-Fi SSID
const char* WIFI_PASSWORD = "";           // Open Wi-Fi (No Password)

// Web Server on Port 80 & WebSocket on /ws
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ==========================================
// 2. Hardware & Sensor Calibration
// ==========================================
const int SENSOR_PIN = 34;                  // ADC pin (GPIO34)

// Voltage Divider Calibration (R1=10k, R2=20k -> Ratio = (10+20)/20 = 1.5)
const float VOLTAGE_DIVIDER_RATIO = 1.50;   

// Transducer Specs
const float SENSOR_MIN_VOLTAGE = 0.50;      // Output voltage at 0 BAR (before divider)
const float SENSOR_MAX_VOLTAGE = 4.50;      // Output voltage at 250 BAR (before divider)
const float SENSOR_MAX_BAR     = 250.0;     // Max pressure range of transducer in BAR

// Cylinder Specifications
const float FULL_CYLINDER_BAR      = 200.0; // Standard full pressure (BAR)
const float EMPTY_CYLINDER_BAR     = 10.0;  // Threshold considered empty (BAR)
const float CYLINDER_WATER_VOL_L   = 10.0;  // Water capacity of cylinder in Liters (e.g. 10L cylinder = 2000L O2 at 200 BAR)

// ==========================================
// 3. Global Variables & Smoothing Filter
// ==========================================
const int NUM_READINGS = 20;
float readings[NUM_READINGS];
int readIndex = 0;
float total = 0.0;
float averageRawVoltage = 0.0;

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL_MS = 500; // Read sensor every 500ms

// Historical tracking for flow rate calculation
float previousBar = -1.0;
unsigned long previousTimeMs = 0;
float estimatedFlowRateLmin = 0.0;

// ==========================================
// 4. Helper Functions: ADC & Physics Math
// ==========================================

// Reads ADC and converts to smoothed actual transducer output voltage
float getSmoothedTransducerVoltage() {
  // Read ESP32 ADC voltage in millivolts using ESP32 calibrated API
  uint32_t rawMv = analogReadMilliVolts(SENSOR_PIN); 
  float espVoltage = rawMv / 1000.0;

  // Multiply by voltage divider ratio to get true sensor output voltage
  float actualSensorVoltage = espVoltage * VOLTAGE_DIVIDER_RATIO;

  // Moving average smoothing filter
  total = total - readings[readIndex];
  readings[readIndex] = actualSensorVoltage;
  total = total + readings[readIndex];
  readIndex = (readIndex + 1) % NUM_READINGS;

  return (total / NUM_READINGS);
}

// Map voltage to pressure in BAR
float voltageToBar(float voltage) {
  if (voltage <= SENSOR_MIN_VOLTAGE) return 0.0;
  if (voltage >= SENSOR_MAX_VOLTAGE) return SENSOR_MAX_BAR;
  
  // Linear interpolation: (V - Vmin) / (Vmax - Vmin) * MaxBar
  float bar = ((voltage - SENSOR_MIN_VOLTAGE) / (SENSOR_MAX_VOLTAGE - SENSOR_MIN_VOLTAGE)) * SENSOR_MAX_BAR;
  return bar;
}

// Convert Percentage (%) to Pressure in BAR
float percentageToBar(float percent) {
  if (percent <= 0.0) return EMPTY_CYLINDER_BAR;
  if (percent >= 100.0) return FULL_CYLINDER_BAR;
  return EMPTY_CYLINDER_BAR + (percent / 100.0) * (FULL_CYLINDER_BAR - EMPTY_CYLINDER_BAR);
}

// Calculate time in minutes required for cylinder to drop to target percentage
float getTimeToTargetPercent(float currentBar, float targetPercent, float flowRateLmin) {
  float targetBar = percentageToBar(targetPercent);
  if (currentBar <= targetBar) return 0.0;
  
  float currentLiters = CYLINDER_WATER_VOL_L * currentBar;
  float targetLiters  = CYLINDER_WATER_VOL_L * targetBar;
  float volumeToConsume = currentLiters - targetLiters;
  
  float activeFlowRate = (flowRateLmin > 0.1) ? flowRateLmin : 5.0; // fallback to 5.0 L/min baseline
  return volumeToConsume / activeFlowRate;
}

// ==========================================
// 5. WebSocket Handler
// ==========================================
void sendSensorDataToAllClients(float currentBar, float oxygenPercent, float volumeLiters, float flowRate, float timeRemainingMins, float timeToTarget20Mins, String statusStr) {
  if (ws.count() == 0) return; // No connected browser clients

  // Construct JSON string
  String json = "{";
  json += "\"pressure_bar\":" + String(currentBar, 1) + ",";
  json += "\"oxygen_percent\":" + String(oxygenPercent, 1) + ",";
  json += "\"remaining_liters\":" + String(volumeLiters, 0) + ",";
  json += "\"flow_rate_lmin\":" + String(flowRate, 1) + ",";
  json += "\"time_remaining_mins\":" + String(timeRemainingMins, 0) + ",";
  json += "\"time_to_target_20_mins\":" + String(timeToTarget20Mins, 0) + ",";
  json += "\"status\":\"" + statusStr + "\"";
  json += "}";

  ws.textAll(json);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

// ==========================================
// 6. Setup & Initialization
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ESP32 Oxygen Cylinder Monitor Starting ---");

  // Initialize ADC Pin
  analogReadResolution(12); // 12-bit ADC (0 - 4095)
  pinMode(SENSOR_PIN, INPUT);

  // Initialize moving average array
  for (int i = 0; i < NUM_READINGS; i++) {
    readings[i] = 0.0;
  }

  // Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected successfully!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  // Attach WebSocket handler
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Fallback REST endpoint for testing via browser
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    float voltage = getSmoothedTransducerVoltage();
    float bar = voltageToBar(voltage);
    float percent = barToPercentage(bar);
    String response = "Pressure: " + String(bar, 1) + " BAR | Oxygen: " + String(percent, 1) + "%";
    request->send(200, "text/plain", response);
  });

  server.begin();
  Serial.println("HTTP & WebSocket server started on port 80");
}

// ==========================================
// 7. Main Loop
// ==========================================
void loop() {
  ws.cleanupClients(); // Clean up disconnected WebSocket sessions

  unsigned long currentMs = millis();
  if (currentMs - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = currentMs;

    // 1. Read Transducer & Convert
    float voltage = getSmoothedTransducerVoltage();
    float currentBar = voltageToBar(voltage);
    float oxygenPercent = barToPercentage(currentBar);

    // 2. Volume calculation (Liter capacity = Water capacity * Pressure in BAR)
    float remainingLiters = CYLINDER_WATER_VOL_L * currentBar;

    // 3. Flow Rate calculation (BAR drop over time)
    if (previousBar > 0.0 && currentMs > previousTimeMs) {
      float timeDiffMinutes = (currentMs - previousTimeMs) / 60000.0;
      float barDrop = previousBar - currentBar;
      
      if (barDrop > 0.1 && timeDiffMinutes > 0) {
        float instantaneousFlowRate = (barDrop * CYLINDER_WATER_VOL_L) / timeDiffMinutes;
        // Smooth flow rate estimate
        estimatedFlowRateLmin = (estimatedFlowRateLmin * 0.8) + (instantaneousFlowRate * 0.2);
      }
    }
    previousBar = currentBar;
    previousTimeMs = currentMs;

    // 4. Time Remaining calculation
    float timeRemainingMins = 0;
    if (estimatedFlowRateLmin > 0.1) {
      timeRemainingMins = remainingLiters / estimatedFlowRateLmin;
    } else {
      // Default baseline estimation if stationary (assuming standard 5 L/min flow rate)
      timeRemainingMins = remainingLiters / 5.0; 
    }

    // 5. Determine System Status
    String statusStr = "NORMAL";
    if (oxygenPercent <= 15.0) {
      statusStr = "CRITICAL";
    } else if (oxygenPercent <= 30.0) {
      statusStr = "LOW";
    }

    // 6. Target Threshold Countdown (e.g. 20%)
    float timeToTarget20Mins = getTimeToTargetPercent(currentBar, 20.0, estimatedFlowRateLmin);

    // 7. Broadcast JSON packet via WebSocket to all connected browser clients
    sendSensorDataToAllClients(currentBar, oxygenPercent, remainingLiters, estimatedFlowRateLmin, timeRemainingMins, timeToTarget20Mins, statusStr);

    // Serial Debug Print
    Serial.printf("Voltage: %.2fV | Pressure: %.1f BAR | Oxygen: %.1f%% | Volume: %.0fL | Status: %s | Clients: %d\n",
                  voltage, currentBar, oxygenPercent, remainingLiters, statusStr.c_str(), ws.count());
  }
}
