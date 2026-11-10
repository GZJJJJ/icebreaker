#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t board_init(void);
esp_err_t board_display_status(const char *status, const char *theme, bool hints_enabled);
esp_err_t board_display_hint(const char *status, const char *theme, bool hints_enabled,
                             bool activity_active, const uint16_t *hint);
esp_err_t board_read_activity_buttons(uint8_t *buttons);
void board_stop_audio(void);
esp_err_t board_start_audio(void);
esp_err_t board_mic_probe(void);
esp_err_t board_read_audio(int16_t *samples, size_t sample_count);
esp_err_t board_play_pcm_mono(const int16_t *samples, size_t sample_count);
esp_err_t board_get_audio_outputs(uint16_t *outputs);
