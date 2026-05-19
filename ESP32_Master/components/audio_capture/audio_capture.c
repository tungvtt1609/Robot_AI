#include "audio_capture.h"

#include <string.h>
#include <limits.h>

#include "audio_common.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

static const char *TAG = "TTL_audio_capture";
static i2s_chan_handle_t s_i2s_rx_chan;
static volatile bool s_capture_paused = false;
static volatile bool s_capture_i2s_disabled = false;

void audio_capture_set_paused(bool paused)
{
    s_capture_paused = paused;
    ESP_LOGI(TAG, "capture %s", paused ? "PAUSED" : "RESUMED");
}

#define MIC_I2S_WS_GPIO ((gpio_num_t)4)
#define MIC_I2S_SCK_GPIO ((gpio_num_t)5)
#define MIC_I2S_SD_GPIO ((gpio_num_t)6)
#define MIC_CHANNEL_LOCK_WARMUP_FRAMES 40
#define MIC_DEFAULT_CHANNEL_INDEX 0
#define MIC_CHANNEL_SWITCH_RATIO_NUM 6
#define MIC_CHANNEL_SWITCH_RATIO_DEN 5

#define MIC_STD_RX_SHIFT 13

static void mic_sd_activity_probe(void)
{
    int transitions = 0;
    int last = gpio_get_level(MIC_I2S_SD_GPIO);

    for (int i = 0; i < 5000; i++) {
        int now = gpio_get_level(MIC_I2S_SD_GPIO);
        if (now != last) {
            transitions++;
            last = now;
        }
        esp_rom_delay_us(20);
    }

    ESP_LOGI(TAG, "mic probe: SD transitions in 100ms = %d", transitions);
    if (transitions == 0) {
        ESP_LOGW(TAG, "mic probe: SD appears static, likely wrong GPIO mapping/wiring or MIC L/R strap/power issue");
    }
}

static void audio_capture_task(void *arg)
{
    RingbufHandle_t out_rb = (RingbufHandle_t)arg;
#if AUDIO_MIC_INPUT_PDM
    static int16_t mic_raw_pcm[AUDIO_PCM_SAMPLES_PER_FRAME];
#else
    static int32_t mic_raw_stereo[AUDIO_PCM_SAMPLES_PER_FRAME * 2];
#endif
    static int16_t pcm[AUDIO_PCM_SAMPLES_PER_FRAME];
    uint32_t frame_count = 0;
#if !AUDIO_MIC_INPUT_PDM
    uint64_t warmup_energy_l = 0;
    uint64_t warmup_energy_r = 0;
    size_t channel_index = MIC_DEFAULT_CHANNEL_INDEX;
    bool channel_locked = false;
#endif

#if AUDIO_MIC_INPUT_PDM
    ESP_LOGI(TAG, "mic capture active (PDM): sample_rate=%d", AUDIO_PCM_SAMPLE_RATE_HZ);
#else
    ESP_LOGI(TAG, "mic capture active (I2S STD): sample_rate=%d", AUDIO_PCM_SAMPLE_RATE_HZ);
#endif

    while (1) {
        if (s_capture_paused) {
            if (!s_capture_i2s_disabled) {
                (void)i2s_channel_disable(s_i2s_rx_chan);
                s_capture_i2s_disabled = true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_capture_i2s_disabled) {
            (void)i2s_channel_enable(s_i2s_rx_chan);
            s_capture_i2s_disabled = false;
        }
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_i2s_rx_chan,
#if AUDIO_MIC_INPUT_PDM
            mic_raw_pcm,
            sizeof(mic_raw_pcm),
#else
            mic_raw_stereo,
            sizeof(mic_raw_stereo),
#endif
            &bytes_read,
            portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %s (0x%x)", esp_err_to_name(err), (unsigned int)err);
            (void)i2s_channel_disable(s_i2s_rx_chan);
            (void)i2s_channel_enable(s_i2s_rx_chan);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

#if AUDIO_MIC_INPUT_PDM
        if (bytes_read < sizeof(mic_raw_pcm)) {
            continue;
        }
#else
        if (bytes_read < sizeof(mic_raw_stereo)) {
            continue;
        }
#endif

        size_t sample_count = AUDIO_PCM_SAMPLES_PER_FRAME;
        if (sample_count > AUDIO_PCM_SAMPLES_PER_FRAME) {
            sample_count = AUDIO_PCM_SAMPLES_PER_FRAME;
        }

#if AUDIO_MIC_INPUT_PDM
        for (size_t i = 0; i < sample_count; i++) {
            int32_t s = mic_raw_pcm[i];
            if (s > INT16_MAX) {
                s = INT16_MAX;
            } else if (s < INT16_MIN) {
                s = INT16_MIN;
            }
            pcm[i] = (int16_t)s;
        }
#else
        int32_t peak_l = 0;
        int32_t peak_r = 0;
        for (size_t i = 0; i < sample_count; i++) {
            int32_t l = mic_raw_stereo[2 * i];
            int32_t r = mic_raw_stereo[2 * i + 1];
            if (l < 0) {
                l = -l;
            }
            if (r < 0) {
                r = -r;
            }
            if (l > peak_l) {
                peak_l = l;
            }
            if (r > peak_r) {
                peak_r = r;
            }
        }

        if (!channel_locked) {
            warmup_energy_l += (uint64_t)peak_l;
            warmup_energy_r += (uint64_t)peak_r;

            if (frame_count >= MIC_CHANNEL_LOCK_WARMUP_FRAMES) {
                uint64_t left_scaled = warmup_energy_l * MIC_CHANNEL_SWITCH_RATIO_NUM;
                uint64_t right_scaled = warmup_energy_r * MIC_CHANNEL_SWITCH_RATIO_NUM;
                uint64_t left_ref = warmup_energy_r * MIC_CHANNEL_SWITCH_RATIO_DEN;
                uint64_t right_ref = warmup_energy_l * MIC_CHANNEL_SWITCH_RATIO_DEN;

                if (right_scaled > left_ref) {
                    channel_index = 1;
                } else if (left_scaled > right_ref) {
                    channel_index = 0;
                } else {
                    channel_index = MIC_DEFAULT_CHANNEL_INDEX;
                }
                channel_locked = true;
                ESP_LOGI(TAG, "capture channel locked to %s (warmup L=%llu R=%llu)",
                         channel_index == 0 ? "L" : "R",
                         (unsigned long long)warmup_energy_l,
                         (unsigned long long)warmup_energy_r);
            } else {
                channel_index = (peak_r > peak_l) ? 1 : 0;
            }
        }

        for (size_t i = 0; i < sample_count; i++) {
            int32_t raw = mic_raw_stereo[2 * i + channel_index];
            int32_t s = (raw >> MIC_STD_RX_SHIFT);
            if (s > INT16_MAX) {
                s = INT16_MAX;
            } else if (s < INT16_MIN) {
                s = INT16_MIN;
            }
            pcm[i] = (int16_t)s;
        }
#endif
        if (sample_count < AUDIO_PCM_SAMPLES_PER_FRAME) {
            memset(&pcm[sample_count], 0, (AUDIO_PCM_SAMPLES_PER_FRAME - sample_count) * sizeof(int16_t));
        }

        int16_t peak = 0;
        for (size_t i = 0; i < AUDIO_PCM_SAMPLES_PER_FRAME; i++) {
            int16_t v = pcm[i];
            if (v < 0) {
                v = (int16_t)(-v);
            }
            if (v > peak) {
                peak = v;
            }
        }

        frame_count++;
        if ((frame_count % 50) == 0) {
#if AUDIO_MIC_INPUT_PDM
            ESP_LOGI(TAG, "capture peak=%d mode=PDM", (int)peak);
#else
            ESP_LOGI(TAG, "capture peak=%d rawL=%ld rawR=%ld use=%s", (int)peak, (long)peak_l, (long)peak_r, channel_index == 0 ? "L" : "R");
#endif
        }

        if (xRingbufferSend(out_rb, pcm, sizeof(pcm), pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "capture ringbuffer full, dropping frame");
        }
    }
}

esp_err_t audio_capture_start(RingbufHandle_t out_rb)
{
    if (out_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "capture mode select: AUDIO_MIC_INPUT_PDM=%d (0=I2S STD, 1=I2S PDM)", AUDIO_MIC_INPUT_PDM);
    ESP_LOGI(TAG, "capture init: creating I2S RX channel");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan), TAG, "create rx channel failed");
    ESP_LOGI(TAG, "capture init: RX channel created");

#if AUDIO_MIC_INPUT_PDM
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_PCM_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_I2S_SCK_GPIO,
            .din = MIC_I2S_SD_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;

    ESP_LOGI(TAG, "capture init: configuring PDM RX mode");
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_i2s_rx_chan, &pdm_cfg), TAG, "init rx pdm mode failed");
    ESP_LOGI(TAG, "capture init: PDM RX mode configured");
#else
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_PCM_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_SCK_GPIO,
            .ws = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_LOGI(TAG, "capture init: configuring I2S STD RX mode");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg), TAG, "init rx std mode failed");
    ESP_LOGI(TAG, "capture init: I2S STD RX mode configured");
#endif

    ESP_LOGI(TAG, "capture init: enabling RX channel");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx_chan), TAG, "enable rx channel failed");
    ESP_LOGI(TAG, "capture init: RX channel enabled");
    mic_sd_activity_probe();

    ESP_LOGI(TAG, "capture init: creating capture task (free_heap=%lu)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    BaseType_t ok = xTaskCreate(audio_capture_task, "audio_cap", 8192, out_rb, 6, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "capture init: xTaskCreate failed (free_heap=%lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return ESP_FAIL;
    }
#if AUDIO_MIC_INPUT_PDM
    ESP_LOGI(TAG, "capture stage started on I2S PDM (CLK=%d DIN=%d)", MIC_I2S_SCK_GPIO, MIC_I2S_SD_GPIO);
#else
    ESP_LOGI(TAG, "capture stage started on I2S STD (WS=%d BCLK=%d SD=%d)", MIC_I2S_WS_GPIO, MIC_I2S_SCK_GPIO, MIC_I2S_SD_GPIO);
#endif
    return ESP_OK;
}