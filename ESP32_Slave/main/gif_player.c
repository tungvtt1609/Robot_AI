#include "gif_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cmn.h"

#define TAG "gif_player"
#define GIF_PLAYER_PATH_MAX 256
#define GIF_LZW_MAX_BITS 12
#define GIF_LZW_TABLE_SIZE (1 << GIF_LZW_MAX_BITS)
#define GIF_PLAYER_LCD_BATCH_LINES 40
#define GIF_PLAYER_DEFAULT_SPEED_PERCENT 100
#define GIF_PLAYER_MAX_SPEED_PERCENT 1000
#define GIF_PLAYER_GPF_BILINEAR_SCALE 0

typedef struct
{
    FILE *file;
    uint8_t block[255];
    uint8_t block_pos;
    uint8_t block_size;
    uint32_t bit_buffer;
    uint8_t bit_count;
    bool ended;
} gif_bit_reader_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t bg_index;
    uint16_t global_table_size;
    uint8_t global_table[256][3];
} gif_header_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    bool interlaced;
    uint16_t table_size;
    uint8_t table[256][3];
} gif_image_t;

typedef struct
{
    uint16_t delay_ms;
    bool transparent;
    uint8_t transparent_index;
} gif_gce_t;

typedef struct
{
    const gif_image_t *image;
    const gif_gce_t *gce;
    const uint8_t (*palette)[3];
    uint16_t *src_line;
    uint16_t *lcd_lines;
    uint16_t src_x;
    uint16_t row_in_pass;
    int batch_y_start;
    uint8_t batch_count;
    uint8_t batch_buffer_index;
    uint8_t pass;
} gif_render_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint16_t target_width;
    uint16_t target_height;
    uint16_t fps;
    uint16_t frame_count;
} gpf_header_t;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static SemaphoreHandle_t s_lcd_tx_done;
static uint16_t s_screen_width;
static uint16_t s_screen_height;
static volatile bool s_stop_requested;
static bool s_running;
static uint16_t s_speed_percent = GIF_PLAYER_DEFAULT_SPEED_PERCENT;
static char s_current_path[GIF_PLAYER_PATH_MAX];
#if GIF_PLAYER_SHOW_FPS
static uint32_t s_fps_x10;
static uint32_t s_fps_frame_count;
static int64_t s_fps_window_start_us;
#endif

static bool gif_player_on_color_done(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    BaseType_t high_task_wakeup = pdFALSE;
    if (s_lcd_tx_done != NULL)
    {
        xSemaphoreGiveFromISR(s_lcd_tx_done, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

static bool gif_player_is_gif_path(const char *path)
{
    const char *ext = path == NULL ? NULL : strrchr(path, '.');
    return ext != NULL && strcasecmp(ext, ".gif") == 0;
}

static bool gif_player_is_supported_path(const char *path)
{
    const char *ext = path == NULL ? NULL : strrchr(path, '.');
    return ext != NULL && (strcasecmp(ext, ".gif") == 0 || strcasecmp(ext, ".gpf") == 0);
}

static bool gif_player_make_sd_path(const char *path, char *out, size_t out_size)
{
    if (path == NULL || path[0] == '\0' || out == NULL || out_size == 0)
    {
        return false;
    }

    int written;
    if (strncmp(path, GIF_PLAYER_SD_MOUNT_POINT, strlen(GIF_PLAYER_SD_MOUNT_POINT)) == 0)
    {
        written = snprintf(out, out_size, "%s", path);
    }
    else if (path[0] == '/')
    {
        written = snprintf(out, out_size, "%s%s", GIF_PLAYER_SD_MOUNT_POINT, path);
    }
    else
    {
        written = snprintf(out, out_size, "%s/%s", GIF_PLAYER_SD_MOUNT_POINT, path);
    }

    return written > 0 && (size_t)written < out_size;
}

static uint16_t read_le16(FILE *file)
{
    uint8_t bytes[2];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
    {
        return 0;
    }
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static bool read_color_table(FILE *file, uint8_t table[256][3], uint16_t size)
{
    return fread(table, 3, size, file) == size;
}

static bool skip_sub_blocks(FILE *file)
{
    uint8_t size = 0;
    do
    {
        if (fread(&size, 1, 1, file) != 1)
        {
            return false;
        }
        if (size > 0 && fseek(file, size, SEEK_CUR) != 0)
        {
            return false;
        }
    } while (size != 0);
    return true;
}

static bool read_gif_header(FILE *file, gif_header_t *header)
{
    uint8_t sig[6];
    uint8_t packed;
    uint8_t aspect;

    memset(header, 0, sizeof(*header));
    if (fread(sig, 1, sizeof(sig), file) != sizeof(sig))
    {
        return false;
    }
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0)
    {
        return false;
    }

    header->width = read_le16(file);
    header->height = read_le16(file);
    if (fread(&packed, 1, 1, file) != 1 ||
        fread(&header->bg_index, 1, 1, file) != 1 ||
        fread(&aspect, 1, 1, file) != 1)
    {
        return false;
    }
    (void)aspect;

    if ((packed & 0x80) == 0)
    {
        ESP_LOGW(TAG, "GIF has no global color table; local frame palettes will be used");
        header->global_table_size = 0;
        return true;
    }

    header->global_table_size = 1U << ((packed & 0x07) + 1);
    return read_color_table(file, header->global_table, header->global_table_size);
}

static bool read_graphic_control(FILE *file, gif_gce_t *gce)
{
    uint8_t block_size;
    uint8_t packed;
    uint16_t delay_cs;
    uint8_t terminator;

    if (fread(&block_size, 1, 1, file) != 1 || block_size != 4 ||
        fread(&packed, 1, 1, file) != 1)
    {
        return false;
    }

    delay_cs = read_le16(file);
    if (fread(&gce->transparent_index, 1, 1, file) != 1 ||
        fread(&terminator, 1, 1, file) != 1)
    {
        return false;
    }

    gce->transparent = (packed & 0x01) != 0;
    gce->delay_ms = delay_cs == 0 ? 30 : delay_cs * 10;
    return terminator == 0;
}

static bool read_image_descriptor(FILE *file, gif_image_t *image)
{
    uint8_t packed;

    memset(image, 0, sizeof(*image));
    image->x = read_le16(file);
    image->y = read_le16(file);
    image->width = read_le16(file);
    image->height = read_le16(file);
    if (fread(&packed, 1, 1, file) != 1)
    {
        return false;
    }

    image->interlaced = (packed & 0x40) != 0;
    if ((packed & 0x80) != 0)
    {
        image->table_size = 1U << ((packed & 0x07) + 1);
        return read_color_table(file, image->table, image->table_size);
    }

    return true;
}

static bool bit_reader_read_byte(gif_bit_reader_t *reader, uint8_t *out)
{
    if (reader->ended)
    {
        return false;
    }

    if (reader->block_pos >= reader->block_size)
    {
        if (fread(&reader->block_size, 1, 1, reader->file) != 1)
        {
            return false;
        }
        if (reader->block_size == 0)
        {
            reader->ended = true;
            return false;
        }
        if (fread(reader->block, 1, reader->block_size, reader->file) != reader->block_size)
        {
            return false;
        }
        reader->block_pos = 0;
    }

    *out = reader->block[reader->block_pos++];
    return true;
}

static int bit_reader_read_code(gif_bit_reader_t *reader, uint8_t code_size)
{
    uint8_t byte;
    while (reader->bit_count < code_size)
    {
        if (!bit_reader_read_byte(reader, &byte))
        {
            return -1;
        }
        reader->bit_buffer |= (uint32_t)byte << reader->bit_count;
        reader->bit_count += 8;
    }

    int code = reader->bit_buffer & ((1U << code_size) - 1U);
    reader->bit_buffer >>= code_size;
    reader->bit_count -= code_size;
    return code;
}

static void bit_reader_discard(gif_bit_reader_t *reader)
{
    uint8_t byte;
    while (bit_reader_read_byte(reader, &byte))
    {
    }
}

static uint16_t rgb888_to_panel_rgb565(const uint8_t rgb[3])
{
    uint16_t rgb565 = ((uint16_t)(rgb[0] & 0xF8) << 8) |
                      ((uint16_t)(rgb[1] & 0xFC) << 3) |
                      ((uint16_t)rgb[2] >> 3);
    return (rgb565 >> 8) | (rgb565 << 8);
}

#if GIF_PLAYER_SHOW_FPS
static uint8_t fps_font_columns(char c, uint8_t columns[5])
{
    static const uint8_t digit_font[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };

    memset(columns, 0, 5);
    if (c >= '0' && c <= '9')
    {
        memcpy(columns, digit_font[c - '0'], 5);
        return 5;
    }

    switch (c)
    {
    case 'F':
        columns[0] = 0x7F;
        columns[1] = 0x09;
        columns[2] = 0x09;
        columns[3] = 0x09;
        columns[4] = 0x01;
        return 5;
    case 'P':
        columns[0] = 0x7F;
        columns[1] = 0x09;
        columns[2] = 0x09;
        columns[3] = 0x09;
        columns[4] = 0x06;
        return 5;
    case 'S':
        columns[0] = 0x46;
        columns[1] = 0x49;
        columns[2] = 0x49;
        columns[3] = 0x49;
        columns[4] = 0x31;
        return 5;
    case ':':
        columns[1] = 0x36;
        columns[2] = 0x36;
        return 3;
    case '.':
        columns[1] = 0x40;
        return 2;
    case ' ':
        return 3;
    default:
        return 0;
    }
}

static void overlay_fps_on_line(uint16_t *line, int y)
{
    enum
    {
        FPS_X = 4,
        FPS_Y = 4,
        FPS_SCALE = 2,
        FPS_FONT_HEIGHT = 7,
    };

    if (y < FPS_Y || y >= FPS_Y + FPS_FONT_HEIGHT * FPS_SCALE)
    {
        return;
    }

    char text[16];
    snprintf(text, sizeof(text), "FPS:%lu.%lu",
             (unsigned long)(s_fps_x10 / 10), (unsigned long)(s_fps_x10 % 10));

    int font_row = (y - FPS_Y) / FPS_SCALE;
    int x = FPS_X;
    for (size_t i = 0; text[i] != '\0' && x < s_screen_width; i++)
    {
        uint8_t columns[5];
        uint8_t width = fps_font_columns(text[i], columns);
        for (uint8_t col = 0; col < width; col++)
        {
            bool on = ((columns[col] >> font_row) & 0x01) != 0;
            if (!on)
            {
                continue;
            }
            int px = x + col * FPS_SCALE;
            for (uint8_t sx = 0; sx < FPS_SCALE; sx++)
            {
                if (px + sx >= 0 && px + sx < s_screen_width)
                {
                    line[px + sx] = 0xFFFF;
                }
            }
        }
        x += (width + 1) * FPS_SCALE;
    }
}

static void update_fps_counter(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_fps_window_start_us == 0)
    {
        s_fps_window_start_us = now_us;
    }

    s_fps_frame_count++;
    int64_t elapsed_us = now_us - s_fps_window_start_us;
    if (elapsed_us >= 1000000)
    {
        s_fps_x10 = (uint32_t)((s_fps_frame_count * 10000000ULL) / elapsed_us);
        s_fps_frame_count = 0;
        s_fps_window_start_us = now_us;
    }
}
#endif

static int render_current_y(gif_render_t *render)
{
    if (!render->image->interlaced)
    {
        return render->row_in_pass;
    }

    static const uint8_t starts[] = {0, 4, 2, 1};
    static const uint8_t steps[] = {8, 8, 4, 2};

    while (render->pass < 4)
    {
        int y = starts[render->pass] + render->row_in_pass * steps[render->pass];
        if (y < render->image->height)
        {
            return y;
        }
        render->pass++;
        render->row_in_pass = 0;
    }

    return -1;
}

static void render_advance_row(gif_render_t *render)
{
    render->src_x = 0;
    render->row_in_pass++;
    if (!render->image->interlaced)
    {
        return;
    }

    while (render->pass < 4 && render_current_y(render) < 0)
    {
        render->pass++;
        render->row_in_pass = 0;
    }
}

static bool wait_lcd_ready(void)
{
    if (s_lcd_tx_done == NULL)
    {
        return false;
    }
    return xSemaphoreTake(s_lcd_tx_done, portMAX_DELAY) == pdTRUE;
}

static bool draw_lcd_area(int y, int height, const uint16_t *lines)
{
    if (height <= 0 || y < 0 || y >= s_screen_height || s_stop_requested)
    {
        return true;
    }
    if (y + height > s_screen_height)
    {
        height = s_screen_height - y;
    }
    if (!wait_lcd_ready())
    {
        return false;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_screen_width, y + height, lines);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_lcd_tx_done);
        ESP_LOGE(TAG, "LCD draw failed at y=%d height=%d: %s", y, height, esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool flush_lcd_batch(gif_render_t *render)
{
    if (render->batch_count == 0)
    {
        return true;
    }

    uint16_t *batch = &render->lcd_lines[(size_t)render->batch_buffer_index *
                                         GIF_PLAYER_LCD_BATCH_LINES * s_screen_width];
    bool ok = draw_lcd_area(render->batch_y_start, render->batch_count, batch);
    render->batch_y_start = -1;
    render->batch_count = 0;
    render->batch_buffer_index ^= 1;
    return ok;
}

static bool append_lcd_line(gif_render_t *render, int dst_y, const uint16_t *line)
{
    if (dst_y < 0 || dst_y >= s_screen_height)
    {
        return true;
    }

    bool starts_new_batch = render->batch_count == 0;
    bool contiguous = !starts_new_batch && dst_y == render->batch_y_start + render->batch_count;
    if (!starts_new_batch && (!contiguous || render->batch_count >= GIF_PLAYER_LCD_BATCH_LINES))
    {
        if (!flush_lcd_batch(render))
        {
            return false;
        }
        starts_new_batch = true;
    }

    if (starts_new_batch)
    {
        render->batch_y_start = dst_y;
    }

    uint16_t *batch = &render->lcd_lines[(size_t)render->batch_buffer_index *
                                         GIF_PLAYER_LCD_BATCH_LINES * s_screen_width];
    memcpy(&batch[render->batch_count * s_screen_width], line, s_screen_width * sizeof(uint16_t));
    render->batch_count++;

    if (render->batch_count >= GIF_PLAYER_LCD_BATCH_LINES)
    {
        return flush_lcd_batch(render);
    }

    return true;
}

static bool flush_source_line(gif_render_t *render, int src_y)
{
    int dst_y_start = ((int)src_y * s_screen_height) / render->image->height;
    int dst_y_end = (((int)src_y + 1) * s_screen_height) / render->image->height;
    uint16_t *scaled_line = &render->lcd_lines[2 * GIF_PLAYER_LCD_BATCH_LINES * s_screen_width];

    if (dst_y_end <= dst_y_start)
    {
        dst_y_end = dst_y_start + 1;
    }

    for (int dst_y = dst_y_start; dst_y < dst_y_end; dst_y++)
    {
        for (uint16_t dst_x = 0; dst_x < s_screen_width; dst_x++)
        {
            uint16_t src_x = ((uint32_t)dst_x * render->image->width) / s_screen_width;
            scaled_line[dst_x] = render->src_line[src_x];
        }
#if GIF_PLAYER_SHOW_FPS
        overlay_fps_on_line(scaled_line, dst_y);
#endif
        if (!append_lcd_line(render, dst_y, scaled_line))
        {
            return false;
        }
    }

    return true;
}

static bool render_pixel(gif_render_t *render, uint8_t color_index)
{
    int src_y = render_current_y(render);
    if (src_y < 0)
    {
        return false;
    }

    if (render->src_x < render->image->width)
    {
        if (render->gce->transparent && color_index == render->gce->transparent_index)
        {
            render->src_line[render->src_x] = 0;
        }
        else
        {
            render->src_line[render->src_x] = rgb888_to_panel_rgb565(render->palette[color_index]);
        }
    }

    render->src_x++;
    if (render->src_x >= render->image->width)
    {
        if (!flush_source_line(render, src_y))
        {
            return false;
        }
        render_advance_row(render);
    }

    return true;
}

static bool decode_image_data(FILE *file, gif_render_t *render)
{
    uint8_t min_code_size;
    uint16_t *prefix = heap_caps_malloc(GIF_LZW_TABLE_SIZE * sizeof(uint16_t), MALLOC_CAP_8BIT);
    uint8_t *suffix = heap_caps_malloc(GIF_LZW_TABLE_SIZE, MALLOC_CAP_8BIT);
    uint8_t *stack = heap_caps_malloc(GIF_LZW_TABLE_SIZE, MALLOC_CAP_8BIT);
    uint16_t stack_size = 0;
    bool ok = false;

    if (prefix == NULL || suffix == NULL || stack == NULL)
    {
        ESP_LOGE(TAG, "Not enough heap for LZW dictionary");
        goto done;
    }

    if (fread(&min_code_size, 1, 1, file) != 1 || min_code_size > 8)
    {
        goto done;
    }

    gif_bit_reader_t reader = {
        .file = file,
    };
    int clear_code = 1 << min_code_size;
    int stop_code = clear_code + 1;
    int available = clear_code + 2;
    int old_code = -1;
    uint8_t code_size = min_code_size + 1;
    uint8_t first = 0;

    for (int i = 0; i < clear_code; i++)
    {
        prefix[i] = 0;
        suffix[i] = i;
    }

    while (!s_stop_requested)
    {
        int code = bit_reader_read_code(&reader, code_size);
        if (code < 0)
        {
            break;
        }
        if (code == clear_code)
        {
            code_size = min_code_size + 1;
            available = clear_code + 2;
            old_code = -1;
            continue;
        }
        if (code == stop_code)
        {
            break;
        }

        int in_code = code;
        if (old_code < 0)
        {
            first = suffix[code];
            if (!render_pixel(render, first))
            {
                goto done;
            }
            old_code = code;
            continue;
        }

        if (code >= available)
        {
            if (stack_size >= GIF_LZW_TABLE_SIZE)
            {
                goto done;
            }
            stack[stack_size++] = first;
            code = old_code;
        }

        while (code >= clear_code)
        {
            if (stack_size >= GIF_LZW_TABLE_SIZE || code >= GIF_LZW_TABLE_SIZE)
            {
                goto done;
            }
            stack[stack_size++] = suffix[code];
            code = prefix[code];
        }

        first = suffix[code];
        if (stack_size >= GIF_LZW_TABLE_SIZE)
        {
            goto done;
        }
        stack[stack_size++] = first;
        while (stack_size > 0)
        {
            if (!render_pixel(render, stack[--stack_size]))
            {
                goto done;
            }
        }

        if (available < GIF_LZW_TABLE_SIZE)
        {
            prefix[available] = old_code;
            suffix[available] = first;
            available++;
            if (available == (1 << code_size) && code_size < GIF_LZW_MAX_BITS)
            {
                code_size++;
            }
        }
        old_code = in_code;
    }

    bit_reader_discard(&reader);
    ok = true;

done:
    free(prefix);
    free(suffix);
    free(stack);
    return ok;
}

static void wait_last_lcd_transfer(void)
{
    if (s_lcd_tx_done != NULL && xSemaphoreTake(s_lcd_tx_done, portMAX_DELAY) == pdTRUE)
    {
        xSemaphoreGive(s_lcd_tx_done);
    }
}

static uint16_t read_u16_from_bytes(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

#if GIF_PLAYER_GPF_BILINEAR_SCALE
static uint16_t rgb565_to_native(uint16_t panel_word)
{
    return (panel_word >> 8) | (panel_word << 8);
}

static uint16_t native_to_panel_rgb565(uint16_t rgb565)
{
    return (rgb565 >> 8) | (rgb565 << 8);
}

static uint16_t blend_rgb565(uint16_t left_panel, uint16_t right_panel, uint16_t weight_right)
{
    uint16_t left = rgb565_to_native(left_panel);
    uint16_t right = rgb565_to_native(right_panel);
    uint16_t weight_left = 256 - weight_right;

    uint16_t lr = (left >> 11) & 0x1F;
    uint16_t lg = (left >> 5) & 0x3F;
    uint16_t lb = left & 0x1F;
    uint16_t rr = (right >> 11) & 0x1F;
    uint16_t rg = (right >> 5) & 0x3F;
    uint16_t rb = right & 0x1F;

    uint16_t r = (lr * weight_left + rr * weight_right + 128) >> 8;
    uint16_t g = (lg * weight_left + rg * weight_right + 128) >> 8;
    uint16_t b = (lb * weight_left + rb * weight_right + 128) >> 8;
    return native_to_panel_rgb565((r << 11) | (g << 5) | b);
}

static void scale_line_bilinear(const uint16_t *src_top, const uint16_t *src_bottom,
                                uint16_t src_width, uint16_t *dst, uint16_t dst_width,
                                uint16_t weight_y)
{
    for (uint16_t dst_x = 0; dst_x < dst_width; dst_x++)
    {
        uint32_t src_x_fp = ((uint32_t)dst_x * (src_width - 1) * 256U) / (dst_width - 1);
        uint16_t src_x = src_x_fp >> 8;
        uint16_t weight_x = src_x_fp & 0xFF;
        uint16_t next_x = src_x + 1 < src_width ? src_x + 1 : src_x;

        uint16_t top = blend_rgb565(src_top[src_x], src_top[next_x], weight_x);
        uint16_t bottom = blend_rgb565(src_bottom[src_x], src_bottom[next_x], weight_x);
        dst[dst_x] = blend_rgb565(top, bottom, weight_y);
    }
}
#endif

static bool read_gpf_header(FILE *file, gpf_header_t *header)
{
    uint8_t raw[16];
    if (fread(raw, 1, sizeof(raw), file) != sizeof(raw))
    {
        return false;
    }
    if (memcmp(raw, "GPF1", 4) != 0)
    {
        return false;
    }

    header->width = read_u16_from_bytes(&raw[4]);
    header->height = read_u16_from_bytes(&raw[6]);
    header->target_width = read_u16_from_bytes(&raw[8]);
    header->target_height = read_u16_from_bytes(&raw[10]);
    header->fps = read_u16_from_bytes(&raw[12]);
    header->frame_count = read_u16_from_bytes(&raw[14]);
    return header->width > 0 && header->height > 0 && header->fps > 0 && header->frame_count > 0;
}

static bool play_gpf_native_file(FILE *file, const char *path, const gpf_header_t *header)
{
    size_t batch_pixels = (size_t)s_screen_width * GIF_PLAYER_LCD_BATCH_LINES;
    uint16_t *lcd_buffers[2] = {
        heap_caps_malloc(batch_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        heap_caps_malloc(batch_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
    };
    if (lcd_buffers[0] == NULL || lcd_buffers[1] == NULL)
    {
        free(lcd_buffers[0]);
        free(lcd_buffers[1]);
        ESP_LOGE(TAG, "Not enough heap for native GPF DMA buffers: %u bytes",
                 (unsigned)(2 * batch_pixels * sizeof(uint16_t)));
        return false;
    }

    ESP_LOGI(TAG, "Playing native GPF fast path %ux%u, frames=%u, fps=%u",
             header->width, header->height, header->frame_count, header->fps);

    uint32_t target_frame_ms = 1000U / header->fps;
    if (target_frame_ms == 0)
    {
        target_frame_ms = 1;
    }

    bool ok = true;
    uint8_t buffer_index = 0;
    while (!s_stop_requested && ok)
    {
        for (uint16_t frame = 0; frame < header->frame_count && !s_stop_requested; frame++)
        {
            int64_t frame_start_us = esp_timer_get_time();

            for (uint16_t y = 0; y < s_screen_height && !s_stop_requested; y += GIF_PLAYER_LCD_BATCH_LINES)
            {
                uint16_t lines = s_screen_height - y;
                if (lines > GIF_PLAYER_LCD_BATCH_LINES)
                {
                    lines = GIF_PLAYER_LCD_BATCH_LINES;
                }

                uint16_t *buffer = lcd_buffers[buffer_index];
                size_t pixels = (size_t)s_screen_width * lines;
                if (fread(buffer, sizeof(uint16_t), pixels, file) != pixels)
                {
                    ok = false;
                    break;
                }

#if GIF_PLAYER_SHOW_FPS
                for (uint16_t line = 0; line < lines; line++)
                {
                    overlay_fps_on_line(&buffer[(size_t)line * s_screen_width], y + line);
                }
#endif

                if (!draw_lcd_area(y, lines, buffer))
                {
                    ok = false;
                    break;
                }
                buffer_index ^= 1;
            }

            wait_last_lcd_transfer();
#if GIF_PLAYER_SHOW_FPS
            update_fps_counter();
#endif
            uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / 1000);
            uint32_t adjusted_frame_ms = (target_frame_ms * 100U) / s_speed_percent;
            if (adjusted_frame_ms > elapsed_ms && !s_stop_requested)
            {
                vTaskDelay(pdMS_TO_TICKS(adjusted_frame_ms - elapsed_ms));
            }
        }
        if (!s_stop_requested && ok)
        {
            fseek(file, 16, SEEK_SET);
        }
    }

    wait_last_lcd_transfer();
    free(lcd_buffers[0]);
    free(lcd_buffers[1]);
    return ok;
}

static bool play_gpf_file(FILE *file, const char *path)
{
    gpf_header_t header;
    if (!read_gpf_header(file, &header))
    {
        ESP_LOGE(TAG, "Invalid GPF stream: %s", path);
        return false;
    }

    if (header.width == s_screen_width && header.height == s_screen_height)
    {
        return play_gpf_native_file(file, path, &header);
    }

    size_t lcd_buffer_lines = 2 * GIF_PLAYER_LCD_BATCH_LINES + 1;
    uint16_t *src_frame = NULL;
    uint16_t *src_line = NULL;
#if GIF_PLAYER_GPF_BILINEAR_SCALE
    src_frame = heap_caps_malloc((size_t)header.width * header.height * sizeof(uint16_t), MALLOC_CAP_8BIT);
#else
    src_line = heap_caps_malloc(header.width * sizeof(uint16_t), MALLOC_CAP_8BIT);
#endif
    uint16_t *x_map = heap_caps_malloc(s_screen_width * sizeof(uint16_t), MALLOC_CAP_8BIT);
    uint16_t *lcd_lines = heap_caps_malloc(s_screen_width * lcd_buffer_lines * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if ((src_frame == NULL && src_line == NULL) || x_map == NULL || lcd_lines == NULL)
    {
        free(src_frame);
        free(src_line);
        free(x_map);
        free(lcd_lines);
        ESP_LOGE(TAG, "Not enough heap for GPF buffers: src=%u bytes, lcd=%u bytes",
                 (unsigned)((size_t)header.width * header.height * sizeof(uint16_t)),
                 (unsigned)(s_screen_width * lcd_buffer_lines * sizeof(uint16_t)));
        return false;
    }

    for (uint16_t dst_x = 0; dst_x < s_screen_width; dst_x++)
    {
        x_map[dst_x] = ((uint32_t)dst_x * header.width) / s_screen_width;
    }

    ESP_LOGI(TAG, "Playing GPF %ux%u -> LCD %ux%u, frames=%u, fps=%u",
             header.width, header.height, s_screen_width, s_screen_height,
             header.frame_count, header.fps);

    bool native_size = header.width == s_screen_width && header.height == s_screen_height;
    uint32_t target_frame_ms = 1000U / header.fps;
    if (target_frame_ms == 0)
    {
        target_frame_ms = 1;
    }

    bool ok = true;
    while (!s_stop_requested && ok)
    {
        for (uint16_t frame = 0; frame < header.frame_count && !s_stop_requested; frame++)
        {
            int64_t frame_start_us = esp_timer_get_time();
            gif_render_t render = {
                .lcd_lines = lcd_lines,
                .batch_y_start = -1,
            };

            if (src_frame != NULL)
            {
                size_t frame_pixels = (size_t)header.width * header.height;
                if (fread(src_frame, sizeof(uint16_t), frame_pixels, file) != frame_pixels)
                {
                    ok = false;
                    break;
                }
            }

            for (uint16_t src_y = 0; src_y < header.height && !s_stop_requested; src_y++)
            {
#if !GIF_PLAYER_GPF_BILINEAR_SCALE
                if (fread(src_line, sizeof(uint16_t), header.width, file) != header.width)
                {
                    ok = false;
                    break;
                }
#endif
                int dst_y_start = ((int)src_y * s_screen_height) / header.height;
                int dst_y_end = (((int)src_y + 1) * s_screen_height) / header.height;
                if (dst_y_end <= dst_y_start)
                {
                    dst_y_end = dst_y_start + 1;
                }

                uint16_t *scaled_line = &lcd_lines[2 * GIF_PLAYER_LCD_BATCH_LINES * s_screen_width];
                for (int dst_y = dst_y_start; dst_y < dst_y_end; dst_y++)
                {
#if GIF_PLAYER_GPF_BILINEAR_SCALE
                    uint32_t src_y_fp = ((uint32_t)dst_y * (header.height - 1) * 256U) / (s_screen_height - 1);
                    uint16_t top_y = src_y_fp >> 8;
                    uint16_t weight_y = src_y_fp & 0xFF;
                    uint16_t bottom_y = top_y + 1 < header.height ? top_y + 1 : top_y;
                    const uint16_t *top_line = &src_frame[top_y * header.width];
                    const uint16_t *bottom_line = &src_frame[bottom_y * header.width];
                    scale_line_bilinear(top_line, bottom_line, header.width, scaled_line,
                                        s_screen_width, weight_y);
#else
                    if (native_size)
                    {
                        memcpy(scaled_line, src_line, s_screen_width * sizeof(uint16_t));
                    }
                    else
                    {
                        for (uint16_t dst_x = 0; dst_x < s_screen_width; dst_x++)
                        {
                            scaled_line[dst_x] = src_line[x_map[dst_x]];
                        }
                    }
#endif
#if GIF_PLAYER_SHOW_FPS
                    overlay_fps_on_line(scaled_line, dst_y);
#endif
                    if (!append_lcd_line(&render, dst_y, scaled_line))
                    {
                        ok = false;
                        break;
                    }
                }
            }

            if (ok)
            {
                ok = flush_lcd_batch(&render);
            }
            wait_last_lcd_transfer();
#if GIF_PLAYER_SHOW_FPS
            update_fps_counter();
#endif

            uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / 1000);
            uint32_t adjusted_frame_ms = (target_frame_ms * 100U) / s_speed_percent;
            if (adjusted_frame_ms > elapsed_ms && !s_stop_requested)
            {
                vTaskDelay(pdMS_TO_TICKS(adjusted_frame_ms - elapsed_ms));
            }
        }
        if (!s_stop_requested && ok)
        {
            fseek(file, 16, SEEK_SET);
        }
    }

    free(src_frame);
    free(src_line);
    free(x_map);
    free(lcd_lines);
    return ok;
}

static bool play_file(FILE *file, const char *path)
{
    gif_header_t header;
    gif_gce_t gce = {
        .delay_ms = 30,
    };
    bool rendered_any_frame = false;

    if (!read_gif_header(file, &header))
    {
        ESP_LOGE(TAG, "Invalid GIF: %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Streaming GIF %ux%u to LCD %ux%u, free heap=%u, largest block=%u",
             header.width, header.height, s_screen_width, s_screen_height,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    while (!s_stop_requested)
    {
        int marker = fgetc(file);
        if (marker == EOF)
        {
            break;
        }
        if (marker == 0x3B)
        {
            break;
        }
        if (marker == 0x21)
        {
            int label = fgetc(file);
            if (label == 0xF9)
            {
                if (!read_graphic_control(file, &gce))
                {
                    return false;
                }
            }
            else if (!skip_sub_blocks(file))
            {
                return false;
            }
            continue;
        }
        if (marker != 0x2C)
        {
            ESP_LOGE(TAG, "Unexpected GIF marker: 0x%02x", marker);
            return false;
        }

        int64_t frame_start_us = esp_timer_get_time();
        gif_image_t image;
        if (!read_image_descriptor(file, &image))
        {
            return false;
        }
        if (image.width == 0 || image.height == 0)
        {
            return false;
        }
        if (image.x != 0 || image.y != 0 || image.width != header.width || image.height != header.height)
        {
            ESP_LOGW(TAG, "Partial GIF frame detected; direct streamer is optimized for full-frame GIFs");
        }

        uint16_t *src_line = heap_caps_malloc(image.width * sizeof(uint16_t), MALLOC_CAP_8BIT);
        size_t lcd_buffer_lines = 2 * GIF_PLAYER_LCD_BATCH_LINES + 1;
        uint16_t *lcd_lines = heap_caps_malloc(s_screen_width * lcd_buffer_lines * sizeof(uint16_t),
                                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (src_line == NULL || lcd_lines == NULL)
        {
            free(src_line);
            free(lcd_lines);
            ESP_LOGE(TAG, "Not enough heap for stream buffers: src=%u bytes, lcd=%u bytes",
                     (unsigned)(image.width * sizeof(uint16_t)),
                     (unsigned)(s_screen_width * lcd_buffer_lines * sizeof(uint16_t)));
            return false;
        }

        const uint8_t (*palette)[3] = NULL;
        if (image.table_size > 0)
        {
            palette = image.table;
        }
        else if (header.global_table_size > 0)
        {
            palette = header.global_table;
        }
        else
        {
            ESP_LOGE(TAG, "GIF frame has no local color table and file has no global color table");
            free(src_line);
            free(lcd_lines);
            return false;
        }

        gif_render_t render = {
            .image = &image,
            .gce = &gce,
            .palette = palette,
            .src_line = src_line,
            .lcd_lines = lcd_lines,
            .batch_y_start = -1,
        };

        bool ok = decode_image_data(file, &render);
        if (ok)
        {
            ok = flush_lcd_batch(&render);
        }
        wait_last_lcd_transfer();
        free(src_line);
        free(lcd_lines);
        if (!ok)
        {
            return false;
        }

        rendered_any_frame = true;
#if GIF_PLAYER_SHOW_FPS
        update_fps_counter();
#endif
        if (gce.delay_ms > 0 && !s_stop_requested)
        {
            uint32_t target_frame_ms = ((uint32_t)gce.delay_ms * 100U) / s_speed_percent;
            uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / 1000);
            if (target_frame_ms > elapsed_ms)
            {
                vTaskDelay(pdMS_TO_TICKS(target_frame_ms - elapsed_ms));
            }
        }
        gce.transparent = false;
        gce.delay_ms = 30;
    }

    return rendered_any_frame;
}

bool gif_player_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     uint16_t screen_width, uint16_t screen_height)
{
    if (panel == NULL || panel_io == NULL || screen_width == 0 || screen_height == 0)
    {
        ESP_LOGE(TAG, "Invalid GIF player init arguments");
        return false;
    }

    s_panel = panel;
    s_panel_io = panel_io;
    s_screen_width = screen_width;
    s_screen_height = screen_height;

    if (s_lcd_tx_done == NULL)
    {
        s_lcd_tx_done = xSemaphoreCreateBinary();
        if (s_lcd_tx_done == NULL)
        {
            ESP_LOGE(TAG, "Failed to create LCD transfer semaphore");
            return false;
        }
        xSemaphoreGive(s_lcd_tx_done);
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = gif_player_on_color_done,
    };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "register LCD callback failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool gif_player_run(const char *path)
{
    if (s_panel == NULL || s_panel_io == NULL)
    {
        ESP_LOGE(TAG, "GIF player is not initialized");
        return false;
    }
    if (!gif_player_is_supported_path(path))
    {
        ESP_LOGE(TAG, "Invalid animation path: %s", path == NULL ? "(null)" : path);
        return false;
    }

    char sd_path[GIF_PLAYER_PATH_MAX];
    if (!gif_player_make_sd_path(path, sd_path, sizeof(sd_path)))
    {
        ESP_LOGE(TAG, "GIF path is too long");
        return false;
    }

    struct stat st;
    if (stat(sd_path, &st) != 0)
    {
        ESP_LOGE(TAG, "GIF file does not exist: %s", sd_path);
        return false;
    }

    FILE *file = fopen(sd_path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open GIF file: %s", sd_path);
        return false;
    }
    char *file_buffer = heap_caps_malloc(16 * 1024, MALLOC_CAP_8BIT);
    if (file_buffer != NULL)
    {
        setvbuf(file, file_buffer, _IOFBF, 16 * 1024);
    }

    snprintf(s_current_path, sizeof(s_current_path), "%s", sd_path);
    s_stop_requested = false;
    s_running = true;
    ESP_LOGI(TAG, "Playing animation with direct LCD streamer: %s", sd_path);

    bool ok = false;
    if (gif_player_is_gif_path(path))
    {
        do
        {
            rewind(file);
            ok = play_file(file, sd_path);
        } while (ok && !s_stop_requested);
    }
    else
    {
        ok = play_gpf_file(file, sd_path);
    }

    fclose(file);
    free(file_buffer);
    wait_last_lcd_transfer();

    s_running = false;
    if (!s_stop_requested && !ok)
    {
        ESP_LOGE(TAG, "Animation streamer failed: %s", sd_path);
    }
    s_current_path[0] = '\0';
    return ok;
}

bool gif_player_stop(void)
{
    s_stop_requested = true;
    return true;
}

bool gif_player_is_running(void)
{
    return s_running;
}

void gif_player_set_speed(uint16_t percent)
{
    if (percent < 100)
    {
        percent = 100;
    }
    else if (percent > GIF_PLAYER_MAX_SPEED_PERCENT)
    {
        percent = GIF_PLAYER_MAX_SPEED_PERCENT;
    }
    s_speed_percent = percent;
}

void gif_player_task(void *arg)
{
    (void)arg;
    while (1)
    {
        switch (play_id)
        {
        case 1:
            /* code */
            gif_player_run("/gif_1.gif");
            break;
        case 2:
            /* code */
            gif_player_run("/gif_2.gif");
            break;
        case 3:
            /* code */
            gif_player_run("/gif_3.gif");
            break;
        case 4:
            /* code */
            gif_player_run("/gif_4.gif");
            break;
        case 5:
            /* code */
            gif_player_run("/gif_5.gif");
            break;
        case 6:
            /* code */
            gif_player_run("/gif_6.gif");
            break;
        case 7:
            /* code */
            gif_player_run("/gif_7.gif");
            break;
        case 8:
            /* code */
            gif_player_run("/gif_8.gif");
            break;
        default:
            gif_player_run("/gif_1.gif");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
