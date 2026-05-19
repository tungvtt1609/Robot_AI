#include "audio_playback.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "audio_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Defined in robot_connectivity/dto_parse.c. True between first audio.play
// chunk of a TTS burst and is_last (or ws disconnect). Used to keep the
// playback task in "playing" mode through mid-burst server pacing gaps.
extern volatile bool g_downlink_burst_active;

static const char *TAG = "TTL_audio_playback";
static i2s_chan_handle_t s_i2s_tx_chan;
static volatile bool s_playback_reset_req = false;
static SemaphoreHandle_t s_i2s_mutex = NULL;
static int32_t s_direct_gain_q12 = 4096;
static uint32_t s_direct_frame_count = 0;
static bool s_i2s_active = true;  // tracks i2s_channel_enable/disable state

#define SPK_I2S_DIN_GPIO ((gpio_num_t)7)
#define SPK_I2S_BCLK_GPIO ((gpio_num_t)15)
#define SPK_I2S_LRC_GPIO ((gpio_num_t)16)
#define SPK_SOFT_VOLUME_NUM 1
#define SPK_SOFT_VOLUME_SHIFT 0
// TTS server already normalizes audio (typical peaks 20000-28000). Push the
// soft-clip threshold close to the s16 rail so we only round off true peaks
// near the limit instead of crushing every loud syllable into distortion.
#define SPK_SOFT_CLIP_START 26000
#define SPK_TARGET_PEAK 20000
#define SPK_PEAK_FLOOR 600
// Keep AGC strictly attenuating-or-unity for loud bursts; only allow up to
// 1.25x boost on quiet sections. Min stays at 1.0x so we never amplify noise
// floor between syllables.
#define SPK_GAIN_Q12_MIN 4096
#define SPK_GAIN_Q12_MAX 5120
#define SPK_OUTPUT_GATE 32

static inline int16_t spk_soft_clip_s16(int32_t v)
{
    int32_t sign = (v < 0) ? -1 : 1;
    int32_t a = (v < 0) ? -v : v;

    if (a > SPK_SOFT_CLIP_START) {
        int32_t over = a - SPK_SOFT_CLIP_START;
        // Soften peaks to avoid harsh crackling when source level suddenly jumps.
        a = SPK_SOFT_CLIP_START + (over / 6);
    }

    if (a > INT16_MAX) {
        a = INT16_MAX;
    }
    return (int16_t)(sign * a);
}

// Prebuffer N frames before starting to drain to I2S so a slow / bursty
// producer (network -> decoder) cannot starve the DMA mid-playback. Each
// frame is 30 ms. The Cloud Run TTS pipeline frequently inserts 700-900 ms
// pacing gaps between TTS chunks (tested: gap of ~900 ms observed at the
// sentence boundary). Prebuffer must cover the worst gap or playback
// audibly stutters mid-utterance, so 30 frames = 900 ms is the floor.
// Once playing, we stay in playing state forever (silence on underrun)
// to avoid double-prebuffer penalty when server gaps mid-burst.
// Must be <= ringbuffer size in audio_pipeline.c (currently 400).
#define SPK_PREBUF_FRAMES 30
#define SPK_PREBUF_BYTES  (SPK_PREBUF_FRAMES * AUDIO_PCM_FRAME_BYTES)
#define SPK_RINGBUF_FRAMES 400
#define SPK_RINGBUF_BYTES  (SPK_RINGBUF_FRAMES * AUDIO_PCM_FRAME_BYTES)// After this many consecutive underrun (silence) frames WHERE THE
// RINGBUFFER IS EMPTY we declare the burst over and drop back to
// prebuffer/idle. Only ringbuffer-empty iterations count, so this is
// reached only when no more data is in transit. 10 frames = 300 ms is
// enough hysteresis to avoid flapping but quick enough that the amp
// shuts down right after the utterance ends.
#define SPK_UNDERRUN_TO_IDLE 10
// When the ringbuffer empties mid-burst (server pacing stall), wait this many
// consecutive empty frames before re-arming prebuffer. Lower than the idle
// threshold so we re-prebuffer fast and turn intermittent micro-stutters
// into a single, more natural pause.
#define SPK_UNDERRUN_TO_REPREBUF 3
static void audio_playback_task(void *arg)
{
    RingbufHandle_t in_rb = (RingbufHandle_t)arg;
    uint32_t frame_count = 0;
    int32_t gain_q12 = SPK_GAIN_Q12_MIN;
    int16_t stereo_buf[AUDIO_PCM_SAMPLES_PER_FRAME * 2];
    bool playing = false;
    int underrun_run = 0;
    // Last audible sample written (mono). Used to ramp down to zero on the
    // first silence frame after real PCM ends, avoiding a DC-step click on
    // the speaker when the burst tail is non-zero.
    int16_t last_sample = 0;
    bool fade_pending = false;
#if AUDIO_OUTPUT_TEST_TONE
    float tone_phase = 0.0f;
    const float tone_step = 2.0f * (float)M_PI * 1000.0f / (float)AUDIO_PCM_SAMPLE_RATE_HZ;
#endif

#if AUDIO_OUTPUT_TEST_TONE
    ESP_LOGW(TAG, "AUDIO_OUTPUT_TEST_TONE is enabled: speaker plays generated 1kHz tone");
#endif

    while (1) {
#if AUDIO_OUTPUT_TEST_TONE
        for (size_t i = 0; i < AUDIO_PCM_SAMPLES_PER_FRAME; i++) {
            int16_t s = (int16_t)(sinf(tone_phase) * 4000.0f);
            stereo_buf[2 * i] = s;
            stereo_buf[2 * i + 1] = s;
            tone_phase += tone_step;
            if (tone_phase >= 2.0f * (float)M_PI) {
                tone_phase -= 2.0f * (float)M_PI;
            }
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            s_i2s_tx_chan,
            stereo_buf,
            sizeof(stereo_buf),
            &bytes_written,
            200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
        }

        frame_count++;
        if (frame_count % 200 == 0) {
            ESP_LOGI(TAG, "test tone output frames: %lu", (unsigned long)frame_count);
        }
        continue;
#else
        // Auto-reset to idle after the buffer fully drained: the underrun
        // counter (incremented in the pcm==NULL path) tells us how many
        // consecutive empty receives we've had. Once it crosses a small
        // threshold we know the burst is done and we can re-arm prebuffer
        // for the next burst, AND power down the amp to silence its hiss.
        if (s_playback_reset_req) {
            s_playback_reset_req = false;
            playing = false;
            ESP_LOGI(TAG, "playback: reset to idle (burst end), played %lu frames",
                     (unsigned long)frame_count);
            frame_count = 0;
        }
        // Prebuffering: stay silent until enough audio has accumulated, then
        // open the valve. This trades ~180 ms of latency for jitter tolerance.
        if (!playing) {
            UBaseType_t free_bytes = xRingbufferGetCurFreeSize(in_rb);
            // Total ringbuffer size is SPK_RINGBUF_BYTES; "used" = total - free.
            size_t used = (free_bytes <= SPK_RINGBUF_BYTES)
                          ? (SPK_RINGBUF_BYTES - free_bytes)
                          : 0;
            if (used < SPK_PREBUF_BYTES) {
                // Not enough buffered yet. Two sub-cases:
                //   - Burst still active on the network side: server is just
                //     pacing slowly, keep I2S running and write silence so we
                //     can resume seamlessly when data arrives.
                //   - Burst ended (idle): write last silence frame, then
                //     disable channel so MAX98357 enters shutdown (no hiss).
                if (g_downlink_burst_active) {
                    if (s_i2s_active) {
                        memset(stereo_buf, 0, sizeof(stereo_buf));
                        size_t w = 0;
                        i2s_channel_write(s_i2s_tx_chan, stereo_buf, sizeof(stereo_buf), &w, 200);
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                if (s_i2s_active) {
                    // If we still have a non-zero tail (e.g. burst ended
                    // exactly on a real-PCM frame and we never wrote a fade
                    // through the underrun path), do a one-frame ramp here
                    // so the I2S DMA closes on a true zero crossing.
                    if (fade_pending && last_sample != 0) {
                        for (size_t i = 0; i < AUDIO_PCM_SAMPLES_PER_FRAME; i++) {
                            int32_t s = (int32_t)last_sample *
                                        (int32_t)(AUDIO_PCM_SAMPLES_PER_FRAME - 1 - i) /
                                        (int32_t)(AUDIO_PCM_SAMPLES_PER_FRAME - 1);
                            stereo_buf[2 * i] = (int16_t)s;
                            stereo_buf[2 * i + 1] = (int16_t)s;
                        }
                        fade_pending = false;
                        last_sample = 0;
                    } else {
                        memset(stereo_buf, 0, sizeof(stereo_buf));
                    }
                    size_t w = 0;
                    i2s_channel_write(s_i2s_tx_chan, stereo_buf, sizeof(stereo_buf), &w, 200);
                    // Let DMA drain (8 desc * 480 frames @16k = 240 ms).
                    vTaskDelay(pdMS_TO_TICKS(260));
                    i2s_channel_disable(s_i2s_tx_chan);
                    s_i2s_active = false;
                    ESP_LOGI(TAG, "playback: idle, I2S disabled (amp shutdown)");
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            // Prebuffer ready - re-enable I2S if it was shut down.
            if (!s_i2s_active) {
                i2s_channel_enable(s_i2s_tx_chan);
                s_i2s_active = true;
            }
            playing = true;
            ESP_LOGI(TAG, "playback: prebuffer ready (%u bytes), opening valve", (unsigned)used);
        }

        size_t item_size = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(in_rb, &item_size,
                                                          pdMS_TO_TICKS(60),
                                                          AUDIO_PCM_FRAME_BYTES);
        if (pcm == NULL) {
            // Underrun. Push silence to keep DMA fed (avoids echo/repeat
            // noise from DMA replaying its last buffer). Drop back to idle
            // ONLY when the ringbuffer is genuinely empty for several
            // consecutive iterations - this avoids losing the tail of an
            // utterance when there's a brief mid-burst server gap that
            // happens to align with us being right at the buffer boundary.
            if (fade_pending && last_sample != 0) {
                // First silence frame after real audio: ramp last_sample
                // -> 0 over the whole frame to avoid a DC-step click.
                for (size_t i = 0; i < AUDIO_PCM_SAMPLES_PER_FRAME; i++) {
                    int32_t s = (int32_t)last_sample *
                                (int32_t)(AUDIO_PCM_SAMPLES_PER_FRAME - 1 - i) /
                                (int32_t)(AUDIO_PCM_SAMPLES_PER_FRAME - 1);
                    stereo_buf[2 * i] = (int16_t)s;
                    stereo_buf[2 * i + 1] = (int16_t)s;
                }
                fade_pending = false;
                last_sample = 0;
            } else {
                memset(stereo_buf, 0, sizeof(stereo_buf));
            }
            size_t written = 0;
            i2s_channel_write(s_i2s_tx_chan, stereo_buf, sizeof(stereo_buf), &written, 200);

            UBaseType_t free_now = xRingbufferGetCurFreeSize(in_rb);
            if (free_now >= SPK_RINGBUF_BYTES - AUDIO_PCM_FRAME_BYTES) {
                // Ringbuffer effectively empty. Only count toward idle when
                // the burst has been signalled complete (is_last/disconnect),
                // otherwise we'd cut the utterance during a server pacing
                // gap (Cloud Run frequently stalls 400-700 ms mid-burst).
                if (!g_downlink_burst_active) {
                    underrun_run++;
                    if (underrun_run >= SPK_UNDERRUN_TO_IDLE) {
                        playing = false;
                        underrun_run = 0;
                        ESP_LOGI(TAG, "playback: ringbuf empty %d frames -> idle (burst end)",
                                 SPK_UNDERRUN_TO_IDLE);
                        frame_count = 0;
                    }
                } else {
                    // Burst still active on the network side. If the stall
                    // persists, drop back to (mid-burst) prebuffer so we get
                    // one clean pause instead of a chain of micro-stutters.
                    underrun_run++;
                    if (underrun_run >= SPK_UNDERRUN_TO_REPREBUF) {
                        playing = false;
                        underrun_run = 0;
                        ESP_LOGI(TAG, "playback: mid-burst stall, re-prebuffering");
                    }
                }
            } else {
                // Buffer has tail data we just couldn't grab in 60ms - keep
                // trying without counting as a real underrun.
                underrun_run = 0;
            }
            continue;
        }
        underrun_run = 0;

        size_t mono_samples = item_size / sizeof(int16_t);

        int32_t frame_peak = 0;
        for (size_t i = 0; i < mono_samples; i++) {
            int32_t a = pcm[i];
            if (a < 0) {
                a = -a;
            }
            if (a > frame_peak) {
                frame_peak = a;
            }
        }

        int32_t target_gain_q12 = SPK_GAIN_Q12_MAX;
        if (frame_peak > SPK_PEAK_FLOOR) {
            target_gain_q12 = (SPK_TARGET_PEAK * SPK_GAIN_Q12_MIN) / frame_peak;
        }
        if (target_gain_q12 < SPK_GAIN_Q12_MIN) {
            target_gain_q12 = SPK_GAIN_Q12_MIN;
        } else if (target_gain_q12 > SPK_GAIN_Q12_MAX) {
            target_gain_q12 = SPK_GAIN_Q12_MAX;
        }

        // Fast attack / slow release keeps output stable while avoiding sudden clipping.
        if (target_gain_q12 < gain_q12) {
            gain_q12 = ((gain_q12 * 3) + target_gain_q12) / 4;
        } else {
            gain_q12 = ((gain_q12 * 15) + target_gain_q12) / 16;
        }

        for (size_t i = 0; i < mono_samples; i++) {
            int32_t scaled = ((int32_t)pcm[i] * SPK_SOFT_VOLUME_NUM) >> SPK_SOFT_VOLUME_SHIFT;
            scaled = (scaled * gain_q12) >> 12;
            int32_t abs_scaled = (scaled < 0) ? -scaled : scaled;
            if (abs_scaled < SPK_OUTPUT_GATE) {
                scaled = 0;
            }
            int16_t s = spk_soft_clip_s16(scaled);
            stereo_buf[2 * i] = s;
            stereo_buf[2 * i + 1] = s;
            if (i + 1 == mono_samples) {
                last_sample = s;
                fade_pending = true;
            }
        }

        // Write the full frame to I2S, retrying any short writes. A partial
        // write means DMA was momentarily full and only accepted part of the
        // frame; the rest of the samples would otherwise be discarded,
        // truncating the audio mid-frame and producing an audible click.
        size_t total_to_write = mono_samples * 2 * sizeof(int16_t);
        size_t total_written = 0;
        const uint8_t *write_ptr = (const uint8_t *)stereo_buf;
        while (total_written < total_to_write) {
            size_t bw = 0;
            esp_err_t werr = i2s_channel_write(
                s_i2s_tx_chan,
                write_ptr + total_written,
                total_to_write - total_written,
                &bw,
                200);
            if (werr != ESP_OK) {
                ESP_LOGW(TAG, "i2s write failed: %s (written=%u/%u)",
                         esp_err_to_name(werr),
                         (unsigned)(total_written + bw), (unsigned)total_to_write);
                total_written += bw;
                break;
            }
            total_written += bw;
            if (bw == 0) {
                // DMA stuck - bail to avoid infinite loop.
                break;
            }
        }
        size_t bytes_written = total_written;

        frame_count++;
        // Log first few frames immediately so we know the chain is alive,
        // then every 10 frames during a burst (typical TTS burst = 20-40 frames).
        if (frame_count <= 3 || (frame_count % 10) == 0) {
            ESP_LOGI(TAG, "playback frame=%lu peak=%ld gain_q12=%ld written=%u",
                     (unsigned long)frame_count,
                     (long)frame_peak, (long)gain_q12,
                     (unsigned)bytes_written);
        }

        vRingbufferReturnItem(in_rb, pcm);
#endif
    }
}

esp_err_t audio_playback_start(RingbufHandle_t in_rb)
{
    if (in_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL), TAG, "create tx channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_PCM_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_BCLK_GPIO,
            .ws = SPK_I2S_LRC_GPIO,
            .dout = SPK_I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg), TAG, "init tx std mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_chan), TAG, "enable tx channel failed");

    ESP_LOGI(TAG, "I2S TX enabled, DMA desc=%d frame=%d", chan_cfg.dma_desc_num, chan_cfg.dma_frame_num);

    s_i2s_mutex = xSemaphoreCreateMutex();
    if (s_i2s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Priority 8: above WS client (7) so I2S writes never get preempted by
    // network bursts, eliminating mid-burst playback jitter.
    BaseType_t ok = xTaskCreate(audio_playback_task, "audio_spk", 4096, in_rb, 8, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "speaker stage started on I2S (DIN=%d BCLK=%d LRC=%d)",
             SPK_I2S_DIN_GPIO, SPK_I2S_BCLK_GPIO, SPK_I2S_LRC_GPIO);
    return ESP_OK;
}

esp_err_t audio_playback_write_pcm(const int16_t *pcm, size_t mono_samples)
{
    if (pcm == NULL || mono_samples == 0 || s_i2s_tx_chan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t frame_peak = 0;
    for (size_t i = 0; i < mono_samples; i++) {
        int32_t a = pcm[i];
        if (a < 0) a = -a;
        if (a > frame_peak) frame_peak = a;
    }

    int32_t target_gain_q12 = SPK_GAIN_Q12_MAX;
    if (frame_peak > SPK_PEAK_FLOOR) {
        target_gain_q12 = (SPK_TARGET_PEAK * SPK_GAIN_Q12_MIN) / frame_peak;
    }
    if (target_gain_q12 < SPK_GAIN_Q12_MIN) target_gain_q12 = SPK_GAIN_Q12_MIN;
    else if (target_gain_q12 > SPK_GAIN_Q12_MAX) target_gain_q12 = SPK_GAIN_Q12_MAX;

    if (target_gain_q12 < s_direct_gain_q12) {
        s_direct_gain_q12 = ((s_direct_gain_q12 * 3) + target_gain_q12) / 4;
    } else {
        s_direct_gain_q12 = ((s_direct_gain_q12 * 15) + target_gain_q12) / 16;
    }
    int32_t gain_q12 = s_direct_gain_q12;

    // Stereo expansion in chunks to bound stack usage.
    enum { CHUNK_SAMPLES = 320 };  // 320 mono -> 640 stereo s16 = 1280 bytes
    int16_t stereo_buf[CHUNK_SAMPLES * 2];

    if (xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t total_written = 0;
    size_t offset = 0;
    while (offset < mono_samples) {
        size_t take = mono_samples - offset;
        if (take > CHUNK_SAMPLES) take = CHUNK_SAMPLES;

        for (size_t i = 0; i < take; i++) {
            int32_t scaled = ((int32_t)pcm[offset + i] * SPK_SOFT_VOLUME_NUM) >> SPK_SOFT_VOLUME_SHIFT;
            scaled = (scaled * gain_q12) >> 12;
            int32_t abs_scaled = (scaled < 0) ? -scaled : scaled;
            if (abs_scaled < SPK_OUTPUT_GATE) scaled = 0;
            int16_t s = spk_soft_clip_s16(scaled);
            stereo_buf[2 * i] = s;
            stereo_buf[2 * i + 1] = s;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_i2s_tx_chan, stereo_buf,
                                          take * 2 * sizeof(int16_t),
                                          &bytes_written, 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed: %s (written=%u)",
                     esp_err_to_name(err), (unsigned)bytes_written);
            xSemaphoreGive(s_i2s_mutex);
            return err;
        }
        total_written += bytes_written;
        offset += take;
    }

    xSemaphoreGive(s_i2s_mutex);

    s_direct_frame_count++;
    if (s_direct_frame_count <= 3 || (s_direct_frame_count % 20) == 0) {
        ESP_LOGI(TAG, "direct frame=%lu samples=%u peak=%ld gain_q12=%ld written=%u",
                 (unsigned long)s_direct_frame_count, (unsigned)mono_samples,
                 (long)frame_peak, (long)gain_q12, (unsigned)total_written);
    }
    return ESP_OK;
}

void audio_playback_reset_state(void)
{
    // Reset AGC + frame counter so the next burst starts with neutral gain
    // and clean log numbering. I2S DMA queue is intentionally NOT purged:
    // it still contains the tail of the just-finished utterance (~240 ms)
    // which we want to play out, not cut off.
    s_playback_reset_req = true;
    s_direct_gain_q12 = SPK_GAIN_Q12_MIN;
    s_direct_frame_count = 0;
}
