#include "audio_codec_opus.h"

#include <string.h>

#include "audio_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

static const char *TAG = "TTL_audio_opus";

// Opus configuration
// Pipeline produces 30 ms PCM frames, but Opus only supports
// 2.5/5/10/20/40/60/80/100/120 ms. We use 20 ms Opus frames to match the
// DTO contract (audio.frame duration_ms=20). The encoder's PCM accumulator
// handles the 30 ms -> 20 ms re-framing transparently: a 30 ms input item
// produces 1 full 20 ms frame + 10 ms leftover that joins the next item.
#define OPUS_FRAME_MS              20
#define OPUS_BITRATE_BPS           24000
#define OPUS_SAMPLES_PER_FRAME     ((AUDIO_PCM_SAMPLE_RATE_HZ * OPUS_FRAME_MS) / 1000)
#define OPUS_PCM_BYTES_PER_FRAME   (OPUS_SAMPLES_PER_FRAME * AUDIO_PCM_CHANNELS * 2)

// Opus internal sample rate is fixed at 48 kHz; granule_position is counted
// at this rate regardless of the encoded sample rate.
#define OPUS_GRANULE_PER_FRAME     ((48000 * OPUS_FRAME_MS) / 1000)

// ---------------------------------------------------------------------------
// OGG/Opus muxer (RFC 3533 + RFC 7845)
// ---------------------------------------------------------------------------

#define OGG_HEADER_TYPE_CONT 0x01
#define OGG_HEADER_TYPE_BOS  0x02
#define OGG_HEADER_TYPE_EOS  0x04

static uint32_t s_ogg_crc_table[256];
static bool     s_ogg_crc_table_ready = false;

static void ogg_crc_table_init(void)
{
    if (s_ogg_crc_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) {
            r = (r & 0x80000000U) ? ((r << 1) ^ 0x04C11DB7U) : (r << 1);
        }
        s_ogg_crc_table[i] = r;
    }
    s_ogg_crc_table_ready = true;
}

static uint32_t ogg_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ s_ogg_crc_table[((crc >> 24) ^ buf[i]) & 0xFF];
    }
    return crc;
}

// Emit one OGG page carrying a single packet (no continuation across pages).
// Returns total bytes written, or 0 if the packet would not fit.
static size_t ogg_emit_page(uint8_t *out, size_t out_cap,
                            uint8_t header_type,
                            uint64_t granule,
                            uint32_t serial,
                            uint32_t page_seq,
                            const uint8_t *pkt, size_t pkt_len)
{
    // Number of lacing segments needed (each up to 255 bytes; if length is a
    // multiple of 255 we add a trailing 0 segment to mark "last 0 bytes").
    size_t seg_count = pkt_len / 255 + 1;
    if (seg_count > 255) {
        // Single packet larger than 255*255 bytes is not supported here.
        return 0;
    }
    size_t total = 27 + seg_count + pkt_len;
    if (total > out_cap) {
        return 0;
    }

    uint8_t *p = out;
    memcpy(p, "OggS", 4); p += 4;
    *p++ = 0;                         // stream_structure_version
    *p++ = header_type;
    // granule_position (LE64)
    for (int i = 0; i < 8; i++) {
        *p++ = (uint8_t)((granule >> (8 * i)) & 0xFF);
    }
    // bitstream_serial_number (LE32)
    *p++ = (uint8_t)(serial & 0xFF);
    *p++ = (uint8_t)((serial >> 8) & 0xFF);
    *p++ = (uint8_t)((serial >> 16) & 0xFF);
    *p++ = (uint8_t)((serial >> 24) & 0xFF);
    // page_sequence_number (LE32)
    *p++ = (uint8_t)(page_seq & 0xFF);
    *p++ = (uint8_t)((page_seq >> 8) & 0xFF);
    *p++ = (uint8_t)((page_seq >> 16) & 0xFF);
    *p++ = (uint8_t)((page_seq >> 24) & 0xFF);
    // checksum placeholder (LE32)
    uint8_t *crc_at = p;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    // segment count
    *p++ = (uint8_t)seg_count;
    // segment table
    size_t remaining = pkt_len;
    for (size_t i = 0; i < seg_count; i++) {
        uint8_t s = (remaining >= 255) ? 255 : (uint8_t)remaining;
        *p++ = s;
        remaining = (remaining >= 255) ? remaining - 255 : 0;
    }
    // payload
    if (pkt_len) memcpy(p, pkt, pkt_len);
    p += pkt_len;

    uint32_t crc = ogg_crc32(out, total);
    crc_at[0] = (uint8_t)(crc & 0xFF);
    crc_at[1] = (uint8_t)((crc >> 8) & 0xFF);
    crc_at[2] = (uint8_t)((crc >> 16) & 0xFF);
    crc_at[3] = (uint8_t)((crc >> 24) & 0xFF);

    return total;
}

// Build the OpusHead identification packet (19 bytes).
static size_t opus_build_head(uint8_t *out, uint32_t input_sample_rate, uint8_t channels)
{
    out[0] = 'O'; out[1] = 'p'; out[2] = 'u'; out[3] = 's';
    out[4] = 'H'; out[5] = 'e'; out[6] = 'a'; out[7] = 'd';
    out[8]  = 1;          // version
    out[9]  = channels;   // channel count
    out[10] = 312 & 0xFF; out[11] = (312 >> 8) & 0xFF;  // pre-skip 312 samples @48kHz (libopus default lookahead)
    out[12] = (uint8_t)(input_sample_rate & 0xFF);
    out[13] = (uint8_t)((input_sample_rate >> 8) & 0xFF);
    out[14] = (uint8_t)((input_sample_rate >> 16) & 0xFF);
    out[15] = (uint8_t)((input_sample_rate >> 24) & 0xFF);
    out[16] = 0; out[17] = 0;  // output gain (LE16)
    out[18] = 0;               // channel mapping family
    return 19;
}

// Build a minimal OpusTags comment packet.
static size_t opus_build_tags(uint8_t *out, size_t out_cap)
{
    static const char vendor[] = "esp32-bot";
    size_t vlen = sizeof(vendor) - 1;
    size_t need = 8 + 4 + vlen + 4;
    if (need > out_cap) return 0;
    uint8_t *p = out;
    memcpy(p, "OpusTags", 8); p += 8;
    *p++ = (uint8_t)(vlen & 0xFF);
    *p++ = (uint8_t)((vlen >> 8) & 0xFF);
    *p++ = (uint8_t)((vlen >> 16) & 0xFF);
    *p++ = (uint8_t)((vlen >> 24) & 0xFF);
    memcpy(p, vendor, vlen); p += vlen;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;  // user comment list length = 0
    return need;
}

// Per-stream OGG state, reset every time the encoder is resumed (= new session).
typedef struct {
    uint32_t serial;
    uint32_t page_seq;
    uint64_t granule;
    bool     headers_emitted;
} ogg_stream_t;

static void ogg_stream_reset(ogg_stream_t *s)
{
    s->serial = esp_random();
    s->page_seq = 0;
    s->granule = 0;
    s->headers_emitted = false;
}

static volatile bool s_encoder_paused = true;
// True when resume() was called -> encoder must start a fresh OGG stream.
static volatile bool s_ogg_reset_pending = true;

// Per-session timing stats. resume() resets them; the encoder task
// accumulates them; take_session() logs and resets them. All in microseconds.
static volatile int64_t  s_session_resume_us   = 0;  // wall-clock when streaming started
static volatile int64_t  s_session_first_enc_us = 0; // wall-clock of first esp_opus_enc_process
static volatile int64_t  s_session_last_enc_us  = 0; // wall-clock of last esp_opus_enc_process
static volatile uint64_t s_session_enc_cpu_us  = 0;  // sum of per-frame encode CPU time
static volatile uint32_t s_session_enc_frames  = 0;  // number of Opus frames produced

// PSRAM-backed accumulator for the entire OGG/Opus byte stream of one session.
// 256 KB is enough for ~80+ s of voice at 24 kbps Opus + OGG framing overhead.
#define SESSION_BUF_BYTES (256 * 1024)
static uint8_t        *s_session_buf = NULL;
static size_t          s_session_len = 0;
static SemaphoreHandle_t s_session_lock = NULL;

void audio_opus_encoder_pause(void) { s_encoder_paused = true; }
void audio_opus_encoder_resume(void)
{
    if (s_session_lock) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        s_session_len = 0;
        xSemaphoreGive(s_session_lock);
    }
    s_ogg_reset_pending = true;
    s_session_resume_us    = esp_timer_get_time();
    s_session_first_enc_us = 0;
    s_session_last_enc_us  = 0;
    s_session_enc_cpu_us   = 0;
    s_session_enc_frames   = 0;
    s_encoder_paused = false;
}
bool audio_opus_encoder_is_paused(void) { return s_encoder_paused; }

esp_err_t audio_opus_encoder_take_session_chunk(const uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) return ESP_ERR_INVALID_ARG;
    if (s_session_lock == NULL) {
        *out_buf = NULL;
        *out_len = 0;
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    *out_buf = s_session_buf;
    *out_len = s_session_len;
    s_session_len = 0;       // logically clear; data still valid until next encoder write
    xSemaphoreGive(s_session_lock);
    return ESP_OK;
}

esp_err_t audio_opus_encoder_take_session(const uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) return ESP_ERR_INVALID_ARG;
    if (s_session_lock == NULL) {
        *out_buf = NULL;
        *out_len = 0;
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    *out_buf = s_session_buf;
    *out_len = s_session_len;
    s_session_len = 0;       // logically clear; data still valid until next resume()
    xSemaphoreGive(s_session_lock);

    // Snapshot + log per-session timing so we can see exactly how much wall
    // clock the streaming took and how much CPU was spent inside the Opus
    // encoder.
    int64_t  resume_us = s_session_resume_us;
    int64_t  first_us  = s_session_first_enc_us;
    int64_t  last_us   = s_session_last_enc_us;
    uint64_t cpu_us    = s_session_enc_cpu_us;
    uint32_t frames    = s_session_enc_frames;
    int64_t  now_us    = esp_timer_get_time();
    if (resume_us != 0 && frames > 0) {
        int64_t wall_total_ms  = (now_us  - resume_us) / 1000;
        int64_t wall_first_ms  = (first_us - resume_us) / 1000;
        int64_t wall_active_ms = (last_us - first_us) / 1000;
        uint64_t cpu_ms        = cpu_us / 1000;
        uint32_t audio_ms      = frames * OPUS_FRAME_MS;
        ESP_LOGI(TAG, "session enc: wall=%lldms (resume->1st=%lldms, 1st->last=%lldms)"
                       " frames=%lu audio=%lums cpu=%llums (avg=%lluus/frame, %lu%% real-time)",
                 (long long)wall_total_ms,
                 (long long)wall_first_ms,
                 (long long)wall_active_ms,
                 (unsigned long)frames,
                 (unsigned long)audio_ms,
                 (unsigned long long)cpu_ms,
                 (unsigned long long)(cpu_us / frames),
                 (unsigned long)((cpu_ms * 100) / (audio_ms ? audio_ms : 1)));
    }
    return ESP_OK;
}

typedef struct {
    RingbufHandle_t in_rb;
    RingbufHandle_t out_rb;
} codec_task_ctx_t;

static void session_buf_append(const uint8_t *data, size_t len)
{
    if (s_session_buf == NULL || len == 0) return;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session_len + len > SESSION_BUF_BYTES) {
        // Drop new data; warn (rate limited via static counter).
        static uint32_t s_overflow = 0;
        s_overflow++;
        if ((s_overflow % 50) == 1) {
            ESP_LOGW(TAG, "session buffer full at %u bytes, dropping (%lu)",
                     (unsigned)s_session_len, (unsigned long)s_overflow);
        }
    } else {
        memcpy(s_session_buf + s_session_len, data, len);
        s_session_len += len;
    }
    xSemaphoreGive(s_session_lock);
}

static void audio_opus_encoder_task(void *arg)
{
    codec_task_ctx_t *ctx = (codec_task_ctx_t *)arg;
    (void)ctx;  // out_rb is unused: stream is buffered in PSRAM until take_session()
    uint32_t sequence_no = 0;

    ogg_crc_table_init();
    ogg_stream_t ogg = {0};
    ogg_stream_reset(&ogg);
    s_ogg_reset_pending = false;

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate      = AUDIO_PCM_SAMPLE_RATE_HZ;
    cfg.channel          = AUDIO_PCM_CHANNELS;
    cfg.bits_per_sample  = 16;
    cfg.bitrate          = OPUS_BITRATE_BPS;
    cfg.frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity       = 0;
    cfg.enable_fec       = false;
    cfg.enable_dtx       = false;
    cfg.enable_vbr       = true;

    void *enc_hd = NULL;
    esp_audio_err_t r = esp_opus_enc_open(&cfg, sizeof(cfg), &enc_hd);
    if (r != ESP_AUDIO_ERR_OK || enc_hd == NULL) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed (%d)", (int)r);
        vTaskDelete(NULL);
        return;
    }

    int in_size = 0;
    int out_size = 0;
    esp_opus_enc_get_frame_size(enc_hd, &in_size, &out_size);
    ESP_LOGI(TAG, "Opus encoder open: %d Hz, %d ch, %d bps, %d ms (in=%d, out=%d)",
             AUDIO_PCM_SAMPLE_RATE_HZ, AUDIO_PCM_CHANNELS, OPUS_BITRATE_BPS, OPUS_FRAME_MS,
             in_size, out_size);

    if (in_size <= 0 || (size_t)in_size > sizeof(((audio_packet_t *)0)->payload) * 4) {
        ESP_LOGE(TAG, "unexpected opus in_size=%d", in_size);
        esp_opus_enc_close(enc_hd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t pcm_buf[OPUS_PCM_BYTES_PER_FRAME];
    size_t pcm_filled = 0;
    uint8_t opus_pkt[256]; // raw Opus packet (20 ms @ 24 kbps fits in ~60-100 bytes; allow headroom)
    uint8_t ogg_page[512]; // scratch for one OGG page (header + segment table + payload)

    while (1) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(ctx->in_rb, &item_size, pdMS_TO_TICKS(100));
        if (item == NULL) {
            continue;
        }

        if (s_encoder_paused) {
            vRingbufferReturnItem(ctx->in_rb, item);
            pcm_filled = 0;
            continue;
        }

        // A new session was requested: rebuild OGG stream from scratch so the
        // server sees a complete OpusHead/OpusTags pair before audio packets.
        if (s_ogg_reset_pending) {
            ogg_stream_reset(&ogg);
            s_ogg_reset_pending = false;
            sequence_no = 0;
        }

        const uint8_t *src = (const uint8_t *)item;
        size_t remaining = item_size;
        while (remaining > 0) {
            size_t space = sizeof(pcm_buf) - pcm_filled;
            size_t copy = remaining < space ? remaining : space;
            memcpy(pcm_buf + pcm_filled, src, copy);
            pcm_filled += copy;
            src        += copy;
            remaining  -= copy;

            if (pcm_filled < (size_t)in_size) {
                break;
            }

            esp_audio_enc_in_frame_t in_f = {
                .buffer = pcm_buf,
                .len    = pcm_filled,
            };
            esp_audio_enc_out_frame_t out_f = {
                .buffer = opus_pkt,
                .len    = sizeof(opus_pkt),
            };
            int64_t enc_t0 = esp_timer_get_time();
            esp_audio_err_t pr = esp_opus_enc_process(enc_hd, &in_f, &out_f);
            int64_t enc_t1 = esp_timer_get_time();
            pcm_filled = 0;

            if (pr != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "esp_opus_enc_process failed (%d)", (int)pr);
                continue;
            }
            if (out_f.encoded_bytes == 0) {
                continue;
            }

            // Per-session timing accounting (read by take_session()).
            if (s_session_first_enc_us == 0) {
                s_session_first_enc_us = enc_t1;
            }
            s_session_last_enc_us = enc_t1;
            s_session_enc_cpu_us += (uint64_t)(enc_t1 - enc_t0);
            s_session_enc_frames++;

            if (!ogg.headers_emitted) {
                uint8_t head_pkt[19];
                size_t  head_len = opus_build_head(head_pkt,
                                                   AUDIO_PCM_SAMPLE_RATE_HZ,
                                                   AUDIO_PCM_CHANNELS);
                size_t w = ogg_emit_page(ogg_page, sizeof(ogg_page),
                                         OGG_HEADER_TYPE_BOS, 0,
                                         ogg.serial, ogg.page_seq++,
                                         head_pkt, head_len);
                if (w == 0) { ESP_LOGW(TAG, "ogg head no space"); continue; }
                session_buf_append(ogg_page, w);

                uint8_t tags_pkt[64];
                size_t  tags_len = opus_build_tags(tags_pkt, sizeof(tags_pkt));
                w = ogg_emit_page(ogg_page, sizeof(ogg_page),
                                  0, 0,
                                  ogg.serial, ogg.page_seq++,
                                  tags_pkt, tags_len);
                if (w == 0) { ESP_LOGW(TAG, "ogg tags no space"); continue; }
                session_buf_append(ogg_page, w);

                ogg.headers_emitted = true;
            }

            ogg.granule += OPUS_GRANULE_PER_FRAME;
            size_t w = ogg_emit_page(ogg_page, sizeof(ogg_page),
                                     0, ogg.granule,
                                     ogg.serial, ogg.page_seq++,
                                     opus_pkt, out_f.encoded_bytes);
            if (w == 0) {
                ESP_LOGW(TAG, "ogg audio page no space (opus=%u)",
                         (unsigned)out_f.encoded_bytes);
                continue;
            }
            session_buf_append(ogg_page, w);

            if ((sequence_no % 50) == 0) {
                ESP_LOGI(TAG, "ogg/opus seq=%lu opus=%u ogg_page=%u dur=%ums total=%u",
                         (unsigned long)sequence_no,
                         (unsigned)out_f.encoded_bytes,
                         (unsigned)w,
                         (unsigned)OPUS_FRAME_MS,
                         (unsigned)s_session_len);
            }
            sequence_no++;
        }

        vRingbufferReturnItem(ctx->in_rb, item);
    }
}

static void audio_opus_decoder_task(void *arg)
{
    codec_task_ctx_t *ctx = (codec_task_ctx_t *)arg;

    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate    = AUDIO_PCM_SAMPLE_RATE_HZ;
    cfg.channel        = AUDIO_PCM_CHANNELS;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID; // assume up to 60 ms
    cfg.self_delimited = false;

    void *dec_hd = NULL;
    esp_audio_err_t r = esp_opus_dec_open(&cfg, sizeof(cfg), &dec_hd);
    if (r != ESP_AUDIO_ERR_OK || dec_hd == NULL) {
        ESP_LOGE(TAG, "esp_opus_dec_open failed (%d)", (int)r);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Opus decoder open: %d Hz, %d ch (max 60 ms PCM out)",
             AUDIO_PCM_SAMPLE_RATE_HZ, AUDIO_PCM_CHANNELS);

    // Output buffer sized for the longest Opus frame we expect (60 ms).
    uint8_t pcm_out[OPUS_PCM_BYTES_PER_FRAME];

    while (1) {
        size_t item_size = 0;
        audio_packet_t *packet = (audio_packet_t *)xRingbufferReceive(ctx->in_rb, &item_size, pdMS_TO_TICKS(100));
        if (packet == NULL) {
            continue;
        }
        if (item_size < sizeof(audio_packet_t)) {
            vRingbufferReturnItem(ctx->in_rb, packet);
            continue;
        }

        size_t payload_len = packet->payload_len;
        if (payload_len > sizeof(packet->payload)) {
            payload_len = sizeof(packet->payload);
        }

        if (packet->codec_id == AUDIO_CODEC_ID_OPUS) {
            esp_audio_dec_in_raw_t raw = {
                .buffer        = packet->payload,
                .len           = (uint32_t)payload_len,
                .consumed      = 0,
                .frame_recover = 0,
            };
            esp_audio_dec_out_frame_t frame = {
                .buffer       = pcm_out,
                .len          = sizeof(pcm_out),
                .needed_size  = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {0};
            r = esp_opus_dec_decode(dec_hd, &raw, &frame, &info);
            if (r != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "esp_opus_dec_decode failed (%d) in_len=%u",
                         (int)r, (unsigned)payload_len);
            } else if (frame.decoded_size > 0) {
                if (xRingbufferSend(ctx->out_rb, pcm_out, frame.decoded_size,
                                    pdMS_TO_TICKS(50)) != pdTRUE) {
                    ESP_LOGW(TAG, "decoder output rb full (opus pcm)");
                }
            }
        } else {
            // PCM passthrough (server PCM frames already packed into 30 ms chunks
            // by the transport layer).
            if (payload_len > 0) {
                if (xRingbufferSend(ctx->out_rb, packet->payload, payload_len,
                                    pdMS_TO_TICKS(50)) != pdTRUE) {
                    ESP_LOGW(TAG, "decoder output rb full (pcm)");
                }
            }
        }
        vRingbufferReturnItem(ctx->in_rb, packet);
    }
}

esp_err_t audio_opus_encoder_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb)
{
    // out_rb is unused (encoded stream is buffered in PSRAM session buffer
    // until take_session()), kept for API compatibility. Allow NULL.
    if (in_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_session_buf == NULL) {
        s_session_buf = (uint8_t *)heap_caps_malloc(SESSION_BUF_BYTES,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_session_buf == NULL) {
            ESP_LOGE(TAG, "session buffer alloc failed (%u bytes PSRAM)", SESSION_BUF_BYTES);
            return ESP_ERR_NO_MEM;
        }
        s_session_len = 0;
    }
    if (s_session_lock == NULL) {
        s_session_lock = xSemaphoreCreateMutex();
        if (s_session_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    static codec_task_ctx_t enc_ctx;
    enc_ctx.in_rb = in_rb;
    enc_ctx.out_rb = out_rb;

    BaseType_t ok = xTaskCreateWithCaps(audio_opus_encoder_task, "audio_enc", 32768, &enc_ctx, 5, NULL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus encode stage started (PSRAM session buffer=%u)", SESSION_BUF_BYTES);
    return ESP_OK;
}

esp_err_t audio_opus_decoder_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb)
{
    if (in_rb == NULL || out_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static codec_task_ctx_t dec_ctx;
    dec_ctx.in_rb = in_rb;
    dec_ctx.out_rb = out_rb;

    BaseType_t ok = xTaskCreateWithCaps(audio_opus_decoder_task, "audio_dec", 32768, &dec_ctx, 5, NULL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus decode stage started");
    return ESP_OK;
}
