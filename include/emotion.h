#pragma once

#include <Arduino.h>

namespace robot {

enum class Emotion {
  Neutral,
  Happy,
  Sad,
  Angry,
  Surprised,
  Thinking
};

Emotion parseEmotion(const String &emotionText);

}  // namespace robot
