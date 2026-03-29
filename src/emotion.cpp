#include "emotion.h"

namespace robot {

Emotion parseEmotion(const String &emotionText) {
  String e = emotionText;
  e.toLowerCase();

  if (e == "happy" || e == "joy") return Emotion::Happy;
  if (e == "sad") return Emotion::Sad;
  if (e == "angry" || e == "mad") return Emotion::Angry;
  if (e == "surprised") return Emotion::Surprised;
  if (e == "thinking") return Emotion::Thinking;

  return Emotion::Neutral;
}

}  // namespace robot
