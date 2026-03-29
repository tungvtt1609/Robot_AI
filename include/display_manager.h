#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "emotion.h"

namespace robot {

class DisplayManager {
 public:
  DisplayManager();

  bool begin();
  void showStatus(const String &line1, const String &line2 = "");
  void renderEmotion(Emotion emotion, const String &subtitle = "");

 private:
  Adafruit_SSD1306 display_;

  void drawCenteredText(const String &text, int16_t y, uint8_t size);
  void drawWrappedText(const String &text, int16_t yStart, uint8_t size);
};

}  // namespace robot
