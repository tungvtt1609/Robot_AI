#include "audio_recorder.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "config.h"

namespace robot {
namespace {
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr size_t kI2sReadChunkBytes = 1024;
}  // namespace

bool AudioRecorder::begin() {
  i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE_HZ,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  i2s_pin_config_t pinConfig = {
      .bck_io_num = I2S_BCLK_PIN,
      .ws_io_num = I2S_WS_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_DATA_IN_PIN,
  };

  if (i2s_driver_install(kI2sPort, &i2sConfig, 0, nullptr) != ESP_OK) {
    Serial.println("Failed to install I2S driver");
    return false;
  }

  if (i2s_set_pin(kI2sPort, &pinConfig) != ESP_OK) {
    Serial.println("Failed to set I2S pins");
    return false;
  }

  if (i2s_zero_dma_buffer(kI2sPort) != ESP_OK) {
    Serial.println("Failed to clear I2S DMA");
    return false;
  }

  return true;
}

bool AudioRecorder::record(int16_t *buffer, size_t sampleCount) const {
  const size_t targetBytes = sampleCount * sizeof(int16_t);
  size_t copiedBytes = 0;

  while (copiedBytes < targetBytes) {
    const size_t requestBytes = min(kI2sReadChunkBytes, targetBytes - copiedBytes);
    size_t bytesRead = 0;

    esp_err_t result = i2s_read(
        kI2sPort,
        reinterpret_cast<uint8_t *>(buffer) + copiedBytes,
        requestBytes,
        &bytesRead,
        pdMS_TO_TICKS(300));

    if (result != ESP_OK) {
      Serial.printf("i2s_read error: %d\n", result);
      return false;
    }

    copiedBytes += bytesRead;
    yield();
  }

  return copiedBytes == targetBytes;
}

}  // namespace robot
