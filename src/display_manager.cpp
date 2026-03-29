#include "display_manager.h"

#include <Wire.h>

#include "config.h"

namespace robot {

DisplayManager::DisplayManager() : display_(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN) {}

bool DisplayManager::begin() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  return display_.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
}

void DisplayManager::drawCenteredText(const String &text, int16_t y, uint8_t size) {
  display_.setTextSize(size);
  display_.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display_.getTextBounds(text, 0, y, &x1, &y1, &w, &h);

  int16_t x = (OLED_WIDTH - static_cast<int16_t>(w)) / 2;
  if (x < 0) x = 0;

  display_.setCursor(x, y);
  display_.print(text);
}

void DisplayManager::drawWrappedText(const String &text, int16_t yStart, uint8_t size) {
  display_.setTextSize(size);
  display_.setTextColor(SSD1306_WHITE);

  const int charsPerLine = OLED_WIDTH / (6 * size);
  String remaining = text;
  int16_t y = yStart;

  while (remaining.length() > 0 && y < OLED_HEIGHT - 8) {
    int split = min(charsPerLine, static_cast<int>(remaining.length()));
    if (split < remaining.length()) {
      int lastSpace = remaining.lastIndexOf(' ', split);
      if (lastSpace > 0) split = lastSpace;
    }

    String line = remaining.substring(0, split);
    remaining = remaining.substring(split);
    remaining.trim();

    display_.setCursor(0, y);
    display_.println(line);
    y += (8 * size);
  }
}

void DisplayManager::showStatus(const String &line1, const String &line2) {
  display_.clearDisplay();
  drawCenteredText(line1, 10, 1);
  if (line2.length() > 0) {
    drawCenteredText(line2, 28, 1);
  }
  display_.display();
}

void DisplayManager::renderEmotion(Emotion emotion, const String &subtitle) {
  display_.clearDisplay();

  display_.drawRoundRect(18, 2, 92, 44, 8, SSD1306_WHITE);

  switch (emotion) {
    case Emotion::Happy:
      display_.drawLine(38, 16, 50, 14, SSD1306_WHITE);
      display_.drawLine(50, 14, 62, 16, SSD1306_WHITE);
      display_.drawLine(66, 16, 78, 14, SSD1306_WHITE);
      display_.drawLine(78, 14, 90, 16, SSD1306_WHITE);
      display_.drawLine(52, 30, 76, 30, SSD1306_WHITE);
      display_.drawLine(54, 31, 74, 31, SSD1306_WHITE);
      display_.drawLine(58, 33, 70, 33, SSD1306_WHITE);
      break;
    case Emotion::Sad:
      display_.drawLine(38, 14, 50, 16, SSD1306_WHITE);
      display_.drawLine(50, 16, 62, 14, SSD1306_WHITE);
      display_.drawLine(66, 14, 78, 16, SSD1306_WHITE);
      display_.drawLine(78, 16, 90, 14, SSD1306_WHITE);
      display_.drawLine(58, 34, 70, 34, SSD1306_WHITE);
      display_.drawLine(54, 36, 74, 36, SSD1306_WHITE);
      display_.drawLine(52, 37, 76, 37, SSD1306_WHITE);
      break;
    case Emotion::Angry:
      display_.drawLine(38, 14, 50, 18, SSD1306_WHITE);
      display_.drawLine(50, 18, 62, 18, SSD1306_WHITE);
      display_.drawLine(66, 18, 78, 18, SSD1306_WHITE);
      display_.drawLine(78, 18, 90, 14, SSD1306_WHITE);
      display_.drawLine(50, 34, 78, 34, SSD1306_WHITE);
      break;
    case Emotion::Surprised:
      display_.fillCircle(50, 16, 4, SSD1306_WHITE);
      display_.fillCircle(78, 16, 4, SSD1306_WHITE);
      display_.drawCircle(64, 33, 6, SSD1306_WHITE);
      break;
    case Emotion::Thinking:
      display_.fillCircle(50, 16, 3, SSD1306_WHITE);
      display_.fillCircle(78, 16, 3, SSD1306_WHITE);
      display_.drawLine(52, 34, 76, 30, SSD1306_WHITE);
      display_.fillCircle(88, 40, 2, SSD1306_WHITE);
      display_.fillCircle(94, 44, 3, SSD1306_WHITE);
      break;
    case Emotion::Neutral:
    default:
      display_.fillCircle(50, 16, 3, SSD1306_WHITE);
      display_.fillCircle(78, 16, 3, SSD1306_WHITE);
      display_.drawLine(52, 32, 76, 32, SSD1306_WHITE);
      break;
  }

  if (subtitle.length() > 0) {
    drawWrappedText(subtitle, 48, 1);
  }

  display_.display();
}

}  // namespace robot
