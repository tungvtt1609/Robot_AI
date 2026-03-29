#pragma once

#include <Arduino.h>

#include "emotion.h"

namespace robot {

class AiClient {
 public:
  String requestSpeechToText(const int16_t *samples, size_t sampleCount) const;
  bool requestChatReply(const String &text, String &reply, Emotion &emotion) const;
};

}  // namespace robot
