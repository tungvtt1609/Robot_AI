#include "ai_client.h"

#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"

namespace robot {

String AiClient::requestSpeechToText(const int16_t *samples, size_t sampleCount) const {
  HTTPClient http;
  http.begin(STT_ENDPOINT);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Sample-Rate", String(SAMPLE_RATE_HZ));
  http.addHeader("X-Format", "pcm_s16le_mono");

  const uint8_t *audioBytes = reinterpret_cast<const uint8_t *>(samples);
  const size_t audioByteLength = sampleCount * sizeof(int16_t);
  int code = http.POST(audioBytes, audioByteLength);

  if (code <= 0) {
    Serial.printf("STT request failed: %s\n", http.errorToString(code).c_str());
    http.end();
    return "";
  }

  if (code != 200) {
    Serial.printf("STT status: %d\n", code);
    Serial.println(http.getString());
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("STT JSON parse error: %s\n", err.c_str());
    return "";
  }

  return doc["text"] | "";
}

bool AiClient::requestChatReply(const String &text, String &reply, Emotion &emotion) const {
  HTTPClient http;
  http.begin(CHAT_ENDPOINT);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument req(512);
  req["text"] = text;
  req["device_id"] = DEVICE_ID;

  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code <= 0) {
    Serial.printf("Chat request failed: %s\n", http.errorToString(code).c_str());
    http.end();
    return false;
  }

  if (code != 200) {
    Serial.printf("Chat status: %d\n", code);
    Serial.println(http.getString());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Chat JSON parse error: %s\n", err.c_str());
    return false;
  }

  reply = doc["reply"] | "";
  emotion = parseEmotion(doc["emotion"] | "neutral");
  return true;
}

}  // namespace robot
