# oura - ESP32 High-Pressure Oxygen Cylinder Monitor & Real-Time Dashboard

An IoT solution for monitoring high-pressure oxygen cylinders in real-time using ESP32, high-pressure transducer, ST7789 TFT display, Node.js server, and a modern web dashboard.

## Features
- Real-time Oxygen Level (%) calculation & visual progress bar
- Transducer Pressure (0 - 250 BAR) monitoring
- Remaining Gas Volume (Liters) calculation
- Estimated Flow Rate (L/min) consumption gradient tracking
- Time Remaining to Empty (0%) countdown
- Target Threshold Countdown (e.g., 20%, 15%, 10%)
- Modern Responsive Web Dashboard (Server-Sent Events)
- 320x240 Landscape TFT Display UI on ESP32

## Components
- **esp-dashboard/**: Node.js telemetry server & responsive web UI (`index.html`)
- **esp-dashboard/esp32/**: PlatformIO C++ firmware with ST7789 TFT display support (`main.cpp`)
- **esp32_oxygen_monitor.ino**: Standalone Arduino sketch
