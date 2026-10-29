#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef enum {
    GATEWAY_READY,
    GATEWAY_THINKING,
    GATEWAY_IDLE,
    GATEWAY_INTERRUPTED,
    GATEWAY_FAILED,
    GATEWAY_DISCONNECTED,
} gateway_event_t;

esp_err_t gateway_client_start(void (*callback)(gateway_event_t event));
esp_err_t gateway_client_begin_turn(void);
esp_err_t gateway_client_end_audio(void);
// Drop pending TTS and stop locally, without waiting for a network request.
void gateway_client_cancel(void);
void gateway_client_playback_started(void);
esp_err_t gateway_client_send_event(const char *event_json);
esp_err_t gateway_client_send_audio(const void *data, size_t length);
int gateway_client_stats(char *buffer, size_t size);
