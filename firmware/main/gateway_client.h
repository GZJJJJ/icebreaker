#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    GATEWAY_READY,
    GATEWAY_THINKING,
    GATEWAY_IDLE,
    GATEWAY_INTERRUPTED,
    GATEWAY_FAILED,
    GATEWAY_DISCONNECTED,
    GATEWAY_ACTIVITY_CHANGED,
} gateway_event_t;

typedef enum {
    ACTIVITY_NONE,
    ACTIVITY_ACTIVE,
    ACTIVITY_PAUSED,
    ACTIVITY_ENDED,
} activity_state_t;

activity_state_t gateway_client_activity_state(void);
bool gateway_client_activity_pending(void);
const char *gateway_client_activity_theme(void);
esp_err_t gateway_client_activity(const char *action, const char *scene, const char *theme);
int gateway_client_activity_stats(char *buffer, size_t size);

esp_err_t gateway_client_start(void (*callback)(gateway_event_t event));
esp_err_t gateway_client_begin_turn(void);
esp_err_t gateway_client_end_audio(void);
// Drop pending TTS and stop locally, without waiting for a network request.
void gateway_client_cancel(void);
void gateway_client_playback_started(void);
esp_err_t gateway_client_send_event(const char *event_json);
esp_err_t gateway_client_send_audio(const void *data, size_t length);
int gateway_client_stats(char *buffer, size_t size);
