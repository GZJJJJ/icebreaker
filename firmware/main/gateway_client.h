#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

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
esp_err_t gateway_client_activity(const char *action, const char *scene, const char *theme,
                                  bool hints_enabled);
int gateway_client_activity_stats(char *buffer, size_t size);

esp_err_t gateway_client_start(void (*callback)(gateway_event_t event));
esp_err_t gateway_client_begin_turn(void);
esp_err_t gateway_client_end_audio(void);
// Drop pending TTS and stop locally, without waiting for a network request.
void gateway_client_cancel(void);
void gateway_client_playback_started(void);
esp_err_t gateway_client_hint_request(uint32_t silence_ms);
bool gateway_client_hint_ready(void);
void gateway_client_hint_dismiss(void);
void gateway_client_copy_hint(uint16_t text[81]);
bool gateway_client_hints_enabled(void);
bool gateway_client_activity_capture_id(char id[33]);
esp_err_t gateway_client_ambient_speech(const char *activity_id);
esp_err_t gateway_client_send_ambient(const char *activity_id, const void *data, size_t length);
esp_err_t gateway_client_send_event(const char *event_json);
esp_err_t gateway_client_send_audio(const void *data, size_t length);
esp_err_t gateway_client_send_camera_snapshot(const char *capture_id, const uint8_t *data, size_t length,
                                              unsigned width, unsigned height);
int gateway_client_stats(char *buffer, size_t size);
