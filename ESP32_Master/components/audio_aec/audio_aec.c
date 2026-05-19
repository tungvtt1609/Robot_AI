#include "audio_aec.h"

#include <limits.h>

#include "audio_common.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "TTL_audio_aec";

#define AEC_SOFT_GAIN 1
#define AEC_HP_ALPHA_Q15 32636
#define AEC_LP_ALPHA_Q15 26000
#define AEC_FLOOR_MARGIN 100
#define AEC_FLOOR_UPDATE_DIV 64
#define AEC_SOFT_CLIP_START 7000
#define AEC_GAIN_Q15_FULL 28672
#define AEC_GAIN_Q15_MED 24576
#define AEC_GAIN_Q15_LOW 20480
#define AEC_GAIN_Q15_MIN 16384
#define AEC_NEAR_PEAK_MED 7000
#define AEC_NEAR_PEAK_HIGH 12000
#define AEC_NEAR_PEAK_MAX 18000
#define AEC_FEEDBACK_HOLD_FRAMES 4
#define AEC_FEEDBACK_DUCK_Q15 18432
#define AEC_FEEDBACK_RELEASE_Q15_STEP 2048
#define AEC_FEEDBACK_TAIL_ATTEN_Q15 24576
#define AEC_VOICE_PEAK_THRESHOLD 1200
#define AEC_VOICE_CONFIRM_FRAMES 6

typedef struct {
    RingbufHandle_t in_rb;
    RingbufHandle_t out_rb;
    int32_t hp_prev_x;
    int32_t hp_prev_y;
    int32_t lp_prev_y;
    int32_t noise_floor;
    int32_t proximity_gain_q15;
    int32_t feedback_duck_q15;
    uint32_t feedback_hold_frames;
    uint32_t frame_count;
    uint32_t voice_frame_count;
    bool mic_ok_reported;
} aec_task_ctx_t;

static inline int16_t clamp_s16(int32_t v)
{
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

static void audio_aec_process_frame(aec_task_ctx_t *ctx, const int16_t *in_pcm, int16_t *out_pcm, size_t samples)
{
    int16_t peak_in = 0;
    int16_t peak_out = 0;

    for (size_t i = 0; i < samples; i++) {
        int32_t a = in_pcm[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak_in) {
            peak_in = (int16_t)a;
        }
    }

    int32_t target_gain_q15 = AEC_GAIN_Q15_FULL;
    if (peak_in > AEC_NEAR_PEAK_MAX) {
        target_gain_q15 = AEC_GAIN_Q15_MIN;
    } else if (peak_in > AEC_NEAR_PEAK_HIGH) {
        target_gain_q15 = AEC_GAIN_Q15_LOW;
    } else if (peak_in > AEC_NEAR_PEAK_MED) {
        target_gain_q15 = AEC_GAIN_Q15_MED;
    }
    // Fast attack, slow release to suppress howling when mic gets too close to speaker.
    if (target_gain_q15 < ctx->proximity_gain_q15) {
        ctx->proximity_gain_q15 = ((ctx->proximity_gain_q15 * 3) + target_gain_q15) / 4;
    } else {
        ctx->proximity_gain_q15 = ((ctx->proximity_gain_q15 * 15) + target_gain_q15) / 16;
    }

    int32_t frame_feedback_gain_q15 = AEC_GAIN_Q15_FULL;
    if (peak_in > AEC_NEAR_PEAK_MAX) {
        frame_feedback_gain_q15 = AEC_GAIN_Q15_MIN;
    } else if (peak_in > AEC_NEAR_PEAK_HIGH) {
        frame_feedback_gain_q15 = AEC_GAIN_Q15_LOW;
    } else if (peak_in > AEC_NEAR_PEAK_MED) {
        frame_feedback_gain_q15 = AEC_GAIN_Q15_MED;
    }

    if (peak_in > AEC_NEAR_PEAK_HIGH) {
        ctx->feedback_duck_q15 = AEC_FEEDBACK_DUCK_Q15;
        ctx->feedback_hold_frames = AEC_FEEDBACK_HOLD_FRAMES;
    } else if (ctx->feedback_hold_frames > 0) {
        ctx->feedback_hold_frames--;
    } else if (ctx->feedback_duck_q15 < 32768) {
        ctx->feedback_duck_q15 += AEC_FEEDBACK_RELEASE_Q15_STEP;
        if (ctx->feedback_duck_q15 > 32768) {
            ctx->feedback_duck_q15 = 32768;
        }
    }

    for (size_t i = 0; i < samples; i++) {
        int32_t x = in_pcm[i];
        int32_t y = ((int32_t)AEC_HP_ALPHA_Q15 * (ctx->hp_prev_y + x - ctx->hp_prev_x)) >> 15;
        ctx->hp_prev_x = x;
        ctx->hp_prev_y = y;

        int32_t ay = (y < 0) ? -y : y;
        if (ctx->noise_floor == 0) {
            ctx->noise_floor = ay;
        }

        int32_t floor_track_limit = ctx->noise_floor + (AEC_FLOOR_MARGIN * 2);
        if (ay < floor_track_limit) {
            ctx->noise_floor = ((ctx->noise_floor * (AEC_FLOOR_UPDATE_DIV - 1)) + ay) / AEC_FLOOR_UPDATE_DIV;
        }

        int32_t suppress_th = ctx->noise_floor + AEC_FLOOR_MARGIN;
        if (suppress_th < 1) {
            suppress_th = 1;
        }

        if (ay < suppress_th) {
            // Soft suppression keeps low-level consonants instead of hard-muting frames.
            y = (y * ay) / suppress_th;
        } else if (ay > (suppress_th * 6)) {
            // Mild compression keeps close-talk speech clearer and reduces clipping/howl risk.
            y = (y * 5) / 6;
        }

        // While in feedback hold, attenuate low-energy tail to avoid ringing without crackling artifacts.
        if (ctx->feedback_hold_frames > 0 && ay < (suppress_th * 3)) {
            y = (y * AEC_FEEDBACK_TAIL_ATTEN_Q15) >> 15;
        }

        y *= AEC_SOFT_GAIN;
        y = (y * ctx->proximity_gain_q15) >> 15;
        y = (y * frame_feedback_gain_q15) >> 15;
        y = (y * ctx->feedback_duck_q15) >> 15;

        // Low-pass smoothing to reduce harsh/scratchy speaker output.
        y = (((int32_t)AEC_LP_ALPHA_Q15 * y) + ((32768 - AEC_LP_ALPHA_Q15) * ctx->lp_prev_y)) >> 15;
        ctx->lp_prev_y = y;

        int32_t ay_gain = (y < 0) ? -y : y;
        if (ay_gain > AEC_SOFT_CLIP_START) {
            int32_t sign = (y < 0) ? -1 : 1;
            int32_t over = ay_gain - AEC_SOFT_CLIP_START;
            // Soft clip avoids harsh limiter artifacts that make speech sound scratchy.
            int32_t clipped = AEC_SOFT_CLIP_START + (over / 4);
            y = sign * clipped;
        }

        out_pcm[i] = clamp_s16(y);

        int16_t ao = out_pcm[i];
        if (ao < 0) {
            ao = (int16_t)(-ao);
        }
        if (ao > peak_out) {
            peak_out = ao;
        }
    }

    if (peak_out >= AEC_VOICE_PEAK_THRESHOLD) {
        if (ctx->voice_frame_count < AEC_VOICE_CONFIRM_FRAMES) {
            ctx->voice_frame_count++;
        }
    } else {
        if (ctx->voice_frame_count > 0) {
            ctx->voice_frame_count--;
        }
        if (peak_out < (AEC_VOICE_PEAK_THRESHOLD / 2)) {
            ctx->mic_ok_reported = false;
        }
    }

    if (!ctx->mic_ok_reported && ctx->voice_frame_count >= AEC_VOICE_CONFIRM_FRAMES) {
        // ESP_LOGI(TAG, "TEST MIC OK");
        ctx->mic_ok_reported = true;
    }

    ctx->frame_count++;
    if ((ctx->frame_count % 50) == 0) {
        ESP_LOGI(TAG, "aec peak_in=%d peak_out=%d floor=%ld gain_q15=%ld duck_q15=%ld hold=%lu",
                 (int)peak_in,
                 (int)peak_out,
                 (long)ctx->noise_floor,
                 (long)ctx->proximity_gain_q15,
                 (long)ctx->feedback_duck_q15,
                 (unsigned long)ctx->feedback_hold_frames);
    }
}

static void audio_aec_task(void *arg)
{
    aec_task_ctx_t *ctx = (aec_task_ctx_t *)arg;
    int16_t out_pcm[AUDIO_PCM_SAMPLES_PER_FRAME];

    while (1) {
        size_t item_size = 0;
        int16_t *item = (int16_t *)xRingbufferReceive(ctx->in_rb, &item_size, pdMS_TO_TICKS(100));
        if (item == NULL) {
            continue;
        }

        size_t samples = item_size / sizeof(int16_t);
        if (samples > AUDIO_PCM_SAMPLES_PER_FRAME) {
            samples = AUDIO_PCM_SAMPLES_PER_FRAME;
        }
        audio_aec_process_frame(ctx, item, out_pcm, samples);

        if (samples < AUDIO_PCM_SAMPLES_PER_FRAME) {
            for (size_t i = samples; i < AUDIO_PCM_SAMPLES_PER_FRAME; i++) {
                out_pcm[i] = 0;
            }
            item_size = AUDIO_PCM_FRAME_BYTES;
        }

        if (xRingbufferSend(ctx->out_rb, out_pcm, item_size, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "AEC output ringbuffer full, dropping frame");
        }
        vRingbufferReturnItem(ctx->in_rb, item);
    }
}

esp_err_t audio_aec_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb)
{
    if (in_rb == NULL || out_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static aec_task_ctx_t ctx;
    ctx.in_rb = in_rb;
    ctx.out_rb = out_rb;
    ctx.hp_prev_x = 0;
    ctx.hp_prev_y = 0;
    ctx.lp_prev_y = 0;
    ctx.noise_floor = 0;
    ctx.proximity_gain_q15 = AEC_GAIN_Q15_FULL;
    ctx.feedback_duck_q15 = 32768;
    ctx.feedback_hold_frames = 0;
    ctx.frame_count = 0;
    ctx.voice_frame_count = 0;
    ctx.mic_ok_reported = false;

    BaseType_t ok = xTaskCreate(audio_aec_task, "audio_aec", 4096, &ctx, 5, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AEC stage started (high-pass + denoise + anti-howling)");
    return ESP_OK;
}
