#include <Arduino.h>

#include "ai_client.h"
#include "audio_recorder.h"
#include "config.h"
#include "display_manager.h"
#include "emotion.h"
#include "wifi_manager.h"

namespace {

robot::DisplayManager g_display;
robot::WiFiManager g_wifi;
robot::AudioRecorder g_recorder;
robot::AiClient g_ai;

unsigned long g_lastCaptureAt = 0;

void handleCycle() {
  const size_t sampleCount = static_cast<size_t>(SAMPLE_RATE_HZ) * RECORD_SECONDS;
  int16_t *audioBuffer = static_cast<int16_t *>(malloc(sampleCount * sizeof(int16_t)));

  if (audioBuffer == nullptr) {
    Serial.println("Not enough memory for audio buffer");
    g_display.showStatus("Memory error");
    delay(1000);
    return;
  }

  g_display.renderEmotion(robot::Emotion::Thinking, "Listening...");
  bool recordOk = g_recorder.record(audioBuffer, sampleCount);

  if (!recordOk) {
    free(audioBuffer);
    g_display.showStatus("Audio record failed");
    delay(1200);
    return;
  }

  g_display.showStatus("Transcribing...");
  String userText = g_ai.requestSpeechToText(audioBuffer, sampleCount);
  free(audioBuffer);

  if (userText.length() == 0) {
    g_display.renderEmotion(robot::Emotion::Neutral, "No speech");
    delay(1000);
    return;
  }

  Serial.print("You: ");
  Serial.println(userText);

  g_display.showStatus("Talking to AI...");
  String reply;
  robot::Emotion emotion = robot::Emotion::Neutral;

  if (!g_ai.requestChatReply(userText, reply, emotion)) {
    g_display.renderEmotion(robot::Emotion::Sad, "Server error");
    delay(1200);
    return;
  }

  Serial.print("AI: ");
  Serial.println(reply);

  g_display.renderEmotion(emotion, reply);
  delay(3000);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!g_display.begin()) {
    Serial.println("SSD1306 allocation failed");
  }

  g_display.showStatus("Robot AI booting...");

  g_display.showStatus("Connecting WiFi", WIFI_SSID);
  if (g_wifi.connectIfNeeded()) {
    g_display.showStatus("WiFi connected");
  } else {
    g_display.showStatus("WiFi failed", "Check credentials");
  }

  if (!g_recorder.begin()) {
    g_display.showStatus("MIC setup failed");
    while (true) {
      delay(1000);
    }
  }

  g_display.renderEmotion(robot::Emotion::Neutral, "Ready");
  delay(1200);
}

void loop() {
  if (!g_wifi.connectIfNeeded()) {
    g_display.showStatus("WiFi reconnect...");
    delay(1000);
    return;
  }

  if (millis() - g_lastCaptureAt < CAPTURE_INTERVAL_MS) {
    delay(50);
    return;
  }
  g_lastCaptureAt = millis();

  handleCycle();
}
