#include "esp_lcd_panel_st7796.h"

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

#define ST7796_CMD_FRMCTR1 0xB1
#define ST7796_CMD_DFUNCTR 0xB6
#define ST7796_CMD_PWCTR1  0xC0
#define ST7796_CMD_PWCTR2  0xC1
#define ST7796_CMD_VMCTR1  0xC5
#define ST7796_CMD_GMCTRP1 0xE0
#define ST7796_CMD_GMCTRN1 0xE1

static const char *TAG = "lcd_panel.st7796";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
} st7796_panel_t;

static esp_err_t panel_st7796_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_st7796_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7796_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7796_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7796_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7796_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_st7796_sleep(esp_lcd_panel_t *panel, bool sleep);

esp_err_t esp_lcd_new_panel_st7796(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7796_panel_t *st7796 = NULL;

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7796 = calloc(1, sizeof(st7796_panel_t));
    ESP_GOTO_ON_FALSE(st7796, ESP_ERR_NO_MEM, err, TAG, "no mem for st7796 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7796->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7796->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        st7796->colmod_val = 0x55;
        st7796->fb_bits_per_pixel = 16;
        break;
    case 18:
        st7796->colmod_val = 0x66;
        st7796->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
    }

    st7796->io = io;
    st7796->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7796->reset_level = panel_dev_config->flags.reset_active_high;
    st7796->base.del = panel_st7796_del;
    st7796->base.reset = panel_st7796_reset;
    st7796->base.init = panel_st7796_init;
    st7796->base.draw_bitmap = panel_st7796_draw_bitmap;
    st7796->base.invert_color = panel_st7796_invert_color;
    st7796->base.mirror = panel_st7796_mirror;
    st7796->base.swap_xy = panel_st7796_swap_xy;
    st7796->base.set_gap = panel_st7796_set_gap;
    st7796->base.disp_on_off = panel_st7796_disp_on_off;
    st7796->base.disp_sleep = panel_st7796_sleep;
    *ret_panel = &st7796->base;
    return ESP_OK;

err:
    if (st7796) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7796);
    }
    return ret;
}

static esp_err_t panel_st7796_del(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    if (st7796->reset_gpio_num >= 0) {
        gpio_reset_pin(st7796->reset_gpio_num);
    }
    free(st7796);
    return ESP_OK;
}

static esp_err_t panel_st7796_reset(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796->io;

    if (st7796->reset_gpio_num >= 0) {
        gpio_set_level(st7796->reset_gpio_num, st7796->reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(st7796->reset_gpio_num, !st7796->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "io tx param failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_st7796_init(esp_lcd_panel_t *panel)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7796->madctl_val,
    }, 1), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7796->colmod_val,
    }, 1), TAG, "io tx param failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_FRMCTR1, (uint8_t[]) {
        0x80, 0x10,
    }, 2), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_DFUNCTR, (uint8_t[]) {
        0x80, 0x02, 0x3B,
    }, 3), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_PWCTR1, (uint8_t[]) {
        0x10, 0x10,
    }, 2), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_PWCTR2, (uint8_t[]) {
        0x41,
    }, 1), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_VMCTR1, (uint8_t[]) {
        0x00, 0x22, 0x80,
    }, 3), TAG, "io tx param failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_GMCTRP1, (uint8_t[]) {
        0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
        0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00,
    }, 15), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_GMCTRN1, (uint8_t[]) {
        0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
        0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00,
    }, 15), TAG, "io tx param failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_NORON, NULL, 0), TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t panel_st7796_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796->io;

    x_start += st7796->x_gap;
    x_end += st7796->x_gap;
    y_start += st7796->y_gap;
    y_end += st7796->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");

    size_t len = (x_end - x_start) * (y_end - y_start) * st7796->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "io tx color failed");
    return ESP_OK;
}

static esp_err_t panel_st7796_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    return esp_lcd_panel_io_tx_param(st7796->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st7796_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    if (mirror_x) {
        st7796->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7796->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7796->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7796->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return esp_lcd_panel_io_tx_param(st7796->io, LCD_CMD_MADCTL, &st7796->madctl_val, 1);
}

static esp_err_t panel_st7796_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    if (swap_axes) {
        st7796->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7796->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return esp_lcd_panel_io_tx_param(st7796->io, LCD_CMD_MADCTL, &st7796->madctl_val, 1);
}

static esp_err_t panel_st7796_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    st7796->x_gap = x_gap;
    st7796->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7796_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    return esp_lcd_panel_io_tx_param(st7796->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_st7796_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
    esp_err_t ret = esp_lcd_panel_io_tx_param(st7796->io, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ret;
}
