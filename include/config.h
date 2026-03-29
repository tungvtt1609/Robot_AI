#pragma once

// =============================
// Wi-Fi
// =============================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =============================
// AI server endpoints
// =============================
// Example: "http://192.168.1.10:8000/api/stt"
#define STT_ENDPOINT "http://YOUR_SERVER_IP:8000/api/stt"
#define CHAT_ENDPOINT "http://YOUR_SERVER_IP:8000/api/chat"

// Optional tag to identify this robot on the server
#define DEVICE_ID "robot-esp32-01"

// =============================
// OLED (SSD1306)
// =============================
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define OLED_RESET_PIN -1
#define OLED_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

// =============================
// I2S microphone (example: INMP441)
// =============================
#define I2S_BCLK_PIN 14
#define I2S_WS_PIN 15
#define I2S_DATA_IN_PIN 32

// =============================
// Audio capture
// =============================
#define SAMPLE_RATE_HZ 16000
#define RECORD_SECONDS 3
#define CAPTURE_INTERVAL_MS 8000
