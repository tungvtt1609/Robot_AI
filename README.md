# Robot AI (ESP32 + PlatformIO)

Robot AI communication with person:
- ESP32 connects to Wi-Fi
- ESP32 records voice from I2S microphone (MIC)
- Audio is sent to AI server for speech-to-text and chat response
- Emotion from AI response is shown on OLED

## Hardware

- ESP32 DevKit
- I2S MIC (example: INMP441)
- OLED SSD1306 128x64 (I2C)

## Wiring (default in config)

### MIC (I2S)
- BCLK/SCK -> GPIO14
- WS/LRCL -> GPIO15
- SD -> GPIO32
- VCC -> 3.3V
- GND -> GND

### OLED (I2C)
- SDA -> GPIO21
- SCL -> GPIO22
- VCC -> 3.3V
- GND -> GND

## Project structure

- [platformio.ini](platformio.ini)
- [include/config.h](include/config.h)
- [include/emotion.h](include/emotion.h)
- [include/display_manager.h](include/display_manager.h)
- [include/wifi_manager.h](include/wifi_manager.h)
- [include/audio_recorder.h](include/audio_recorder.h)
- [include/ai_client.h](include/ai_client.h)
- [src/main.cpp](src/main.cpp)
- [src/emotion.cpp](src/emotion.cpp)
- [src/display_manager.cpp](src/display_manager.cpp)
- [src/wifi_manager.cpp](src/wifi_manager.cpp)
- [src/audio_recorder.cpp](src/audio_recorder.cpp)
- [src/ai_client.cpp](src/ai_client.cpp)

## Module overview

- `DisplayManager`: all OLED drawing and emotion rendering
- `WiFiManager`: Wi-Fi connect/reconnect logic
- `AudioRecorder`: I2S MIC setup and PCM recording
- `AiClient`: HTTP communication with STT and Chat APIs
- `Emotion`: shared emotion enum + parser
- `main.cpp`: app orchestration/state flow only

## Chức năng từng file

### Root

- [platformio.ini](platformio.ini)
	- Cấu hình PlatformIO cho ESP32 (`framework`, `board`, `monitor_speed`, thư viện phụ thuộc).

### include/

- [include/config.h](include/config.h)
	- Khai báo cấu hình hệ thống: Wi-Fi, API endpoint, chân OLED, chân I2S MIC, tham số audio.
	- Đây là file bạn chỉnh sửa đầu tiên khi đổi phần cứng/mạng.

- [include/emotion.h](include/emotion.h)
	- Định nghĩa kiểu dữ liệu `Emotion` dùng chung toàn bộ project.
	- Khai báo hàm `parseEmotion()` để map chuỗi từ server thành enum.

- [include/display_manager.h](include/display_manager.h)
	- Interface cho module OLED.
	- Cung cấp `begin()`, `showStatus()`, `renderEmotion()`.

- [include/wifi_manager.h](include/wifi_manager.h)
	- Interface cho module Wi-Fi.
	- Cung cấp `connectIfNeeded()` để kết nối hoặc tự reconnect.

- [include/audio_recorder.h](include/audio_recorder.h)
	- Interface cho module thu âm I2S.
	- Cung cấp `begin()` (khởi tạo I2S) và `record()` (ghi PCM vào buffer).

- [include/ai_client.h](include/ai_client.h)
	- Interface cho module gọi AI server.
	- Cung cấp `requestSpeechToText()` và `requestChatReply()`.

### src/

- [src/main.cpp](src/main.cpp)
	- Entry point của firmware (`setup()` / `loop()`).
	- Điều phối luồng chính: kiểm tra Wi-Fi -> thu âm -> STT -> chat AI -> hiển thị cảm xúc.
	- Không chứa logic phần cứng chi tiết để dễ maintain.

- [src/emotion.cpp](src/emotion.cpp)
	- Cài đặt `parseEmotion()`.
	- Chuẩn hóa giá trị cảm xúc trả về từ server.

- [src/display_manager.cpp](src/display_manager.cpp)
	- Cài đặt toàn bộ logic vẽ OLED.
	- Vẽ trạng thái hệ thống và khuôn mặt cảm xúc theo `Emotion`.

- [src/wifi_manager.cpp](src/wifi_manager.cpp)
	- Cài đặt kết nối Wi-Fi station mode.
	- Xử lý retry và in log trạng thái mạng qua Serial.

- [src/audio_recorder.cpp](src/audio_recorder.cpp)
	- Cài đặt driver I2S RX cho MIC.
	- Đọc dữ liệu PCM 16-bit mono từ MIC vào buffer.

- [src/ai_client.cpp](src/ai_client.cpp)
	- Cài đặt HTTP client tới STT API và Chat API.
	- Parse JSON response, trả về text và `Emotion` cho `main.cpp`.

## Setup

1. Open this folder in VS Code with PlatformIO extension.
2. Edit Wi-Fi and server URLs in [include/config.h](include/config.h).
3. Build and upload with PlatformIO.
4. Open serial monitor at 115200.

## Server API contract

### 1) Speech-to-text endpoint
- URL: `POST /api/stt`
- Content-Type: `application/octet-stream`
- Body: raw PCM `s16le`, mono, 16 kHz
- Headers:
	- `X-Sample-Rate: 16000`
	- `X-Format: pcm_s16le_mono`
- Response JSON example:

```json
{
	"text": "xin chào robot"
}
```

### 2) Chat endpoint
- URL: `POST /api/chat`
- Content-Type: `application/json`
- Request JSON example:

```json
{
	"text": "xin chào robot",
	"device_id": "robot-esp32-01"
}
```

- Response JSON example:

```json
{
	"reply": "Xin chào, mình rất vui được gặp bạn!",
	"emotion": "happy"
}
```

Supported emotion values:
- `neutral`
- `happy`
- `sad`
- `angry`
- `surprised`
- `thinking`

## Notes

- Current firmware shows AI reply text and emotion on OLED.
- If you need voice output from robot to person, add a speaker path (I2S DAC/amplifier) and TTS endpoint on server.
