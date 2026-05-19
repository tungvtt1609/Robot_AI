#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define GIF_PLAYER_DEFAULT_PATH "/gif_2.gpf"
#define GIF_PLAYER_SD_MOUNT_POINT "/sdcard"
#define GIF_PLAYER_SHOW_FPS 1

bool gif_player_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     uint16_t screen_width, uint16_t screen_height);
bool gif_player_run(const char *path);
bool gif_player_stop(void);
bool gif_player_is_running(void);
void gif_player_set_speed(uint16_t percent);
void gif_player_task(void *arg);