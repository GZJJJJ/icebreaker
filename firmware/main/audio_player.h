#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_PLAYER_STARTED,
    AUDIO_PLAYER_FINISHED,
    AUDIO_PLAYER_INTERRUPTED,
    AUDIO_PLAYER_FAILED,
} audio_player_event_t;

esp_err_t audio_player_init(void (*callback)(audio_player_event_t event));
// Ownership transfers only on ESP_OK. PCM is mono, PCM16LE, 24 kHz.
esp_err_t audio_player_submit(uint8_t *pcm, size_t length);
void audio_player_stop(void);
bool audio_player_busy(void);
uint32_t audio_player_bytes_written(void);
uint32_t audio_player_failures(void);
uint32_t audio_player_interrupt_us(void);
