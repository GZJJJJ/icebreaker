#include "gateway_client.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"
#include "audio_player.h"
#include "board.h"
#include "usb_diag.h"
#include "hint_state.h"

static const char *TAG = "gateway";
static esp_websocket_client_handle_t s_client;
static atomic_bool s_connected;
static atomic_bool s_upload_enabled;
static atomic_int s_activity_state = ACTIVITY_NONE;
static atomic_bool s_activity_pending;
static atomic_int s_activity_theme;
static atomic_bool s_hints_enabled;
static char s_activity_id[33];
static const char *THEMES[] = {"travel", "film", "reading", "general"};
static const char *s_activity_scene = "social";
static unsigned s_history_turns;
static hint_state_t s_hint;
static SemaphoreHandle_t s_gate;
static void (*s_callback)(gateway_event_t event);
static unsigned s_turn;
static char s_session_id[32];
static bool s_awaiting_start;
static bool s_accept_reply;
static bool s_collecting_tts;
static bool s_wait_metrics;
static const char *s_reply_stage = "idle";
static char s_last_error[40];
static uint32_t s_timings_ms[6];
static int s_cancel_ms;
static atomic_uint s_reply_started_ms;
static atomic_uint s_playback_wait_ms;
static char *s_rx_payload;
static size_t s_rx_capacity;
static size_t s_rx_length;

// TTS audio is collected for the whole reply, then written in one continuous
// pass. Playing each 100 ms frame as it arrives leaves gaps between frames
// that are audible as buzzing.
static uint8_t *s_tts_buf;
static size_t s_tts_len;
static size_t s_tts_cap;

static volatile uint32_t s_frames_rx;
static volatile uint32_t s_bytes_queued;
static volatile int s_last_peak;
static volatile uint32_t s_decode_failures;
static volatile uint32_t s_play_failures;

#define TTS_BUFFER_LIMIT (900 * 1024)

int gateway_client_stats(char *buffer, size_t size)
{
    uint16_t outputs = 0;
    const int xio_output = board_get_audio_outputs(&outputs) == ESP_OK ? outputs : -1;
    xSemaphoreTake(s_gate, portMAX_DELAY);
    const int length = snprintf(buffer, size,
                    "{\"frames_rx\":%u,\"bytes_queued\":%u,\"bytes_played\":%u,"
                    "\"last_peak\":%d,\"decode_failures\":%u,\"play_failures\":%u,"
                    "\"spiram_free\":%u,\"xio_output\":%d,\"connected\":%s,"
                    "\"player_busy\":%s,\"interrupt_us\":%u,\"reply_stage\":\"%s\","
                    "\"last_error\":\"%s\",\"cancel_ms\":%d,\"playback_wait_ms\":%u,\"timings_ms\":{"
                    "\"asr\":%u,\"llm_first\":%u,\"llm\":%u,\"tts\":%u,\"send\":%u,\"total\":%u}}\n",
                    (unsigned)s_frames_rx, (unsigned)s_bytes_queued,
                    (unsigned)audio_player_bytes_written(), s_last_peak,
                    (unsigned)s_decode_failures,
                    (unsigned)(s_play_failures + audio_player_failures()),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), xio_output,
                    s_connected ? "true" : "false", audio_player_busy() ? "true" : "false",
                    (unsigned)audio_player_interrupt_us(),
                    audio_player_busy() ? "playback" : s_reply_stage,
                    s_last_error, s_cancel_ms, (unsigned)s_playback_wait_ms,
                    (unsigned)s_timings_ms[0], (unsigned)s_timings_ms[1],
                    (unsigned)s_timings_ms[2], (unsigned)s_timings_ms[3],
                    (unsigned)s_timings_ms[4], (unsigned)s_timings_ms[5]);
    xSemaphoreGive(s_gate);
    return length;
}

static void tts_buffer_reset(void)
{
    free(s_tts_buf);
    s_tts_buf = NULL;
    s_tts_len = 0;
    s_tts_cap = 0;
    s_collecting_tts = false;
}

static bool tts_buffer_append(const uint8_t *data, size_t length)
{
    if (length == 0 || s_tts_len + length > TTS_BUFFER_LIMIT) {
        return false;
    }
    if (s_tts_len + length > s_tts_cap) {
        size_t next_cap = s_tts_cap ? s_tts_cap : 64 * 1024;
        while (next_cap < s_tts_len + length) {
            next_cap *= 2;
        }
        if (next_cap > TTS_BUFFER_LIMIT) {
            next_cap = TTS_BUFFER_LIMIT;
        }
        uint8_t *next = heap_caps_realloc(s_tts_buf, next_cap,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (next == NULL) {
            next = heap_caps_realloc(s_tts_buf, next_cap, MALLOC_CAP_8BIT);
        }
        if (next == NULL) {
            return false;
        }
        s_tts_buf = next;
        s_tts_cap = next_cap;
        s_bytes_queued += (uint32_t)length;
        memcpy(s_tts_buf + s_tts_len, data, length);
        s_tts_len += length;
        return true;
    }
    memcpy(s_tts_buf + s_tts_len, data, length);
    s_tts_len += length;
    s_bytes_queued += (uint32_t)length;
    return true;
}

static void cancel_locked(void)
{
    hint_state_clear(&s_hint);
    s_accept_reply = false;
    s_awaiting_start = false;
    s_upload_enabled = false;
    s_wait_metrics = false;
    s_reply_started_ms = 0;
    s_reply_stage = "interrupted";
    tts_buffer_reset();
    audio_player_stop();
}

void gateway_client_cancel(void)
{
    if (s_gate == NULL) {
        audio_player_stop();
        return;
    }
    xSemaphoreTake(s_gate, portMAX_DELAY);
    cancel_locked();
    xSemaphoreGive(s_gate);
}

void gateway_client_playback_started(void)
{
    // Called while the player holds its own gate; never acquire the Gateway gate here.
    const unsigned started = atomic_exchange(&s_reply_started_ms, 0);
    s_playback_wait_ms = started == 0 ? 0 : (unsigned)(esp_timer_get_time() / 1000) - started;
}

static void notify(gateway_event_t event)
{
    if (s_callback != NULL) {
        s_callback(event);
    }
}

static void fail_reply(void)
{
    s_activity_pending = false;
    const bool wait_metrics = s_wait_metrics;
    cancel_locked();
    s_wait_metrics = wait_metrics;
    s_reply_stage = "error";
    notify(GATEWAY_FAILED);
}

static void handle_json(cJSON *root)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(name)) {
        return;
    }
    if (strcmp(name->valuestring, "gateway.ready") == 0) {
        s_activity_state = ACTIVITY_NONE;
        s_activity_pending = false;
        s_history_turns = 0;
        s_hints_enabled = false;
        hint_state_clear(&s_hint);
        s_reply_stage = "idle";
#ifdef ICEBREAKER_TEXT_UI_TEST
        gateway_client_send_event("{\"event\":\"device.ready\",\"camera_capture\":true,\"ambient_context\":false,\"text_ui_test\":true}");
#else
        gateway_client_send_event("{\"event\":\"device.ready\",\"camera_capture\":true,\"ambient_context\":true}");
#endif
        notify(GATEWAY_READY);
        return;
    }
    if (strcmp(name->valuestring, "activity.state") == 0 && cJSON_IsString(state)) {
        const char *states[] = {"none", "active", "paused", "ended"};
        for (int i = 0; i < 4; ++i) {
            if (strcmp(state->valuestring, states[i]) == 0) {
                s_activity_state = i;
                break;
            }
        }
        const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
        if (cJSON_IsString(theme)) {
            for (int i = 0; i < 4; ++i) {
                if (strcmp(theme->valuestring, THEMES[i]) == 0) s_activity_theme = i;
            }
        }
        const cJSON *scene = cJSON_GetObjectItemCaseSensitive(root, "scene");
        const char *scenes[] = {"social", "date", "meeting"};
        if (cJSON_IsString(scene)) {
            for (int i = 0; i < 3; ++i) {
                if (strcmp(scene->valuestring, scenes[i]) == 0) s_activity_scene = scenes[i];
            }
        }
        const cJSON *history = cJSON_GetObjectItemCaseSensitive(root, "history_turns");
        s_history_turns = cJSON_IsNumber(history) && history->valueint >= 0 ? history->valueint : 0;
        const cJSON *hints = cJSON_GetObjectItemCaseSensitive(root, "hints_enabled");
        s_hints_enabled = cJSON_IsTrue(hints);
        const cJSON *activity_id = cJSON_GetObjectItemCaseSensitive(root, "activity_id");
        s_activity_id[0] = '\0';
        if (cJSON_IsString(activity_id) && strlen(activity_id->valuestring) == 32 &&
            strspn(activity_id->valuestring, "0123456789abcdef") == 32) {
            memcpy(s_activity_id, activity_id->valuestring, sizeof(s_activity_id));
        }
        s_activity_pending = false;
        hint_state_clear(&s_hint);
        s_reply_stage = "idle";
        if (s_activity_state == ACTIVITY_PAUSED || s_activity_state == ACTIVITY_ENDED)
            usb_diag_cancel_capture();
        notify(GATEWAY_ACTIVITY_CHANGED);
        return;
    }
    if (strcmp(name->valuestring, "hint.suggestion") == 0) {
        if (s_activity_state != ACTIVITY_ACTIVE || !s_hints_enabled || s_activity_pending) return;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const cJSON *suggestion = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (!cJSON_IsNumber(id) || id->valuedouble != s_hint.request_id ||
            !cJSON_IsString(suggestion)) return;
        if (!hint_state_accept(&s_hint, s_hint.request_id, suggestion->valuestring)) return;
        s_reply_stage = "hint_ready";
        notify(GATEWAY_ACTIVITY_CHANGED);
        return;
    }
    if (strcmp(name->valuestring, "hint.error") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (cJSON_IsNumber(id) && id->valuedouble == s_hint.request_id) s_hint.pending = false;
        return;
    }
    if (strcmp(name->valuestring, "ambient.speech") == 0) {
        hint_state_clear(&s_hint);
        notify(GATEWAY_ACTIVITY_CHANGED);
        return;
    }
    if (strcmp(name->valuestring, "camera.capture") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "capture_id");
        const cJSON *consent = cJSON_GetObjectItemCaseSensitive(root, "consent");
        if (!cJSON_IsTrue(consent) || !cJSON_IsString(id)) return;
        if (usb_diag_request_capture(id->valuestring) == ESP_OK) {
            char response[128];
            snprintf(response, sizeof(response), "{\"event\":\"camera.capture.started\",\"capture_id\":\"%.32s\"}", id->valuestring);
            gateway_client_send_event(response);
        } else if (strlen(id->valuestring) == 32 && strspn(id->valuestring, "0123456789abcdef") == 32) {
            char response[128];
            snprintf(response, sizeof(response), "{\"event\":\"camera.capture.error\",\"code\":\"busy\",\"capture_id\":\"%.32s\"}", id->valuestring);
            gateway_client_send_event(response);
        }
        return;
    }
    if (strcmp(name->valuestring, "camera.cancel") == 0) {
        usb_diag_cancel_capture();
        return;
    }
    if (strcmp(name->valuestring, "activity.error") == 0) {
        s_activity_pending = false;
        strlcpy(s_last_error, "activity_rejected", sizeof(s_last_error));
        fail_reply();
        return;
    }
    if (strcmp(name->valuestring, "session.state") == 0 && cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "interrupted") == 0 || strcmp(state->valuestring, "ended") == 0)
            usb_diag_cancel_capture();
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (strcmp(state->valuestring, "listening") == 0 && s_awaiting_start &&
            cJSON_IsString(id) && strcmp(id->valuestring, s_session_id) == 0) {
            // Ordered acknowledgement: all events from the old turn preceded this.
            s_awaiting_start = false;
            s_accept_reply = true;
            const cJSON *cancel_ms = cJSON_GetObjectItemCaseSensitive(root, "cancel_ms");
            s_cancel_ms = cJSON_IsNumber(cancel_ms) ? cancel_ms->valueint : -1;
            s_reply_stage = "listening";
        } else if (s_accept_reply && strcmp(state->valuestring, "thinking") == 0) {
            notify(GATEWAY_THINKING);
        } else if (s_accept_reply && strcmp(state->valuestring, "idle") == 0) {
            s_accept_reply = false;
            s_upload_enabled = false;
            tts_buffer_reset();
            // Server transmission is finished; local playback may still be running.
            notify(GATEWAY_IDLE);
        } else if (s_accept_reply && strcmp(state->valuestring, "interrupted") == 0) {
            cancel_locked();
            notify(GATEWAY_INTERRUPTED);
        }
        return;
    }
    if (strcmp(name->valuestring, "reply.metrics") == 0 && s_wait_metrics) {
        const cJSON *timings = cJSON_GetObjectItemCaseSensitive(root, "timings_ms");
        const char *keys[] = {"asr", "llm_first", "llm", "tts", "send", "total"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(timings, keys[i]);
            if (cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= UINT32_MAX) {
                s_timings_ms[i] = (uint32_t)value->valuedouble;
            }
        }
        s_wait_metrics = false;
        return;
    }
    if (!s_accept_reply) {
        return;
    }
    if (strcmp(name->valuestring, "reply.stage") == 0) {
        const cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "stage");
        const char *stages[] = {"asr", "llm", "tts", "sending"};
        if (cJSON_IsString(stage)) {
            for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
                if (strcmp(stage->valuestring, stages[i]) == 0) {
                    s_reply_stage = stages[i];
                    break;
                }
            }
        }
    } else if (strcmp(name->valuestring, "asr.final") == 0 && cJSON_IsString(text)) {
        ESP_LOGI(TAG, "ASR received: %u bytes", (unsigned)strlen(text->valuestring));
    } else if (strcmp(name->valuestring, "llm.delta") == 0 && cJSON_IsString(text)) {
        ESP_LOGD(TAG, "LLM delta: %u bytes", (unsigned)strlen(text->valuestring));
    } else if (strcmp(name->valuestring, "tts.start") == 0) {
        const cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
        const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
        tts_buffer_reset();
        s_reply_stage = "receiving";
        if (!cJSON_IsNumber(rate) || rate->valueint != 24000 ||
            !cJSON_IsString(format) || strcmp(format->valuestring, "pcm_s16le") != 0) {
            s_decode_failures++;
            fail_reply();
        } else {
            s_collecting_tts = true;
        }
    } else if (strcmp(name->valuestring, "tts.audio") == 0) {
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
        size_t length = 0;
        int16_t pcm[1600];
        if (!s_collecting_tts || !cJSON_IsString(audio) ||
            mbedtls_base64_decode((uint8_t *)pcm, sizeof(pcm), &length,
                                  (const unsigned char *)audio->valuestring,
                                  strlen(audio->valuestring)) != 0 ||
            length % sizeof(int16_t) != 0 || !tts_buffer_append((uint8_t *)pcm, length)) {
            s_decode_failures++;
            fail_reply();
            return;
        }
        const int16_t *samples = (const int16_t *)pcm;
        int peak = 0;
        for (size_t i = 0; i < length / sizeof(int16_t); ++i) {
            const int value = samples[i] < 0 ? -samples[i] : samples[i];
            if (value > peak) {
                peak = value;
            }
        }
        s_last_peak = peak;
        s_frames_rx++;
    } else if (strcmp(name->valuestring, "tts.end") == 0) {
        if (!s_collecting_tts || audio_player_submit(s_tts_buf, s_tts_len) != ESP_OK) {
            s_play_failures++;
            fail_reply();
        } else {
            // The player now owns this allocation. Never free or reuse it here.
            s_tts_buf = NULL;
            tts_buffer_reset();
            s_reply_stage = "ready";
        }
    } else if (strcmp(name->valuestring, "error") == 0) {
        s_activity_pending = false;
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        // Store only a restricted diagnostic token, never arbitrary server text.
        const char *value = cJSON_IsString(code) ? code->valuestring : "gateway_error";
        size_t i = 0;
        while (value[i] != '\0' && i + 1 < sizeof(s_last_error) &&
               ((value[i] >= 'a' && value[i] <= 'z') || value[i] == '_')) {
            s_last_error[i] = value[i];
            ++i;
        }
        s_last_error[i] = '\0';
        ESP_LOGW(TAG, "gateway returned error");
        fail_reply();
    }
}

static void gateway_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WEBSOCKET_EVENTS) {
        return;
    }
    esp_websocket_event_data_t *event = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        s_connected = false;
        s_activity_state = ACTIVITY_NONE;
        s_activity_pending = false;
        s_history_turns = 0;
        hint_state_clear(&s_hint);
        usb_diag_cancel_capture();
        gateway_client_cancel();
        notify(GATEWAY_DISCONNECTED);
        ESP_LOGW(TAG, "disconnected; client will reconnect");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DATA && event->op_code != 0x0 && event->op_code != 0x1) {
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || event->data_len <= 0) {
        return;
    }
    if (event->payload_len >= 16 * 1024) {
        return;
    }

    if (event->payload_offset == 0) {
        s_rx_length = 0;
        const size_t required = (event->payload_len > 0 ? event->payload_len : event->data_len) + 1;
        if (required > 16 * 1024) {
            s_rx_length = 0;
            return;
        }
        if (required > s_rx_capacity) {
            char *next = realloc(s_rx_payload, required);
            if (next == NULL) {
                free(s_rx_payload);
                s_rx_payload = NULL;
                s_rx_capacity = 0;
                s_rx_length = 0;
                return;
            }
            s_rx_payload = next;
            s_rx_capacity = required;
        }
    }
    if (s_rx_payload == NULL || s_rx_length + event->data_len + 1 > s_rx_capacity) {
        return;
    }
    memcpy(s_rx_payload + s_rx_length, event->data_ptr, event->data_len);
    s_rx_length += event->data_len;
    if (!event->fin || event->payload_offset + event->data_len < event->payload_len) {
        return;
    }
    s_rx_payload[s_rx_length] = '\0';
    cJSON *root = cJSON_Parse(s_rx_payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid JSON event");
        return;
    }

    xSemaphoreTake(s_gate, portMAX_DELAY);
    handle_json(root);
    xSemaphoreGive(s_gate);
    cJSON_Delete(root);
}

esp_err_t gateway_client_start(void (*callback)(gateway_event_t event))
{
    s_callback = callback;
    s_gate = xSemaphoreCreateMutex();
    if (s_gate == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (CONFIG_ICEBREAKER_GATEWAY_URI[0] == '\0') {
        ESP_LOGW(TAG, "Gateway URI is empty; WebSocket client disabled");
        return ESP_OK;
    }
    esp_websocket_client_config_t config = {
        .uri = CONFIG_ICEBREAKER_GATEWAY_URI,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
        .task_stack = 8192,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                       gateway_event_handler, NULL),
                        TAG, "register WebSocket handler failed");
    return esp_websocket_client_start(s_client);
}

esp_err_t gateway_client_begin_turn(void)
{
    if (!s_connected || audio_player_busy() || s_activity_pending ||
        s_activity_state == ACTIVITY_PAUSED || s_activity_state == ACTIVITY_ENDED) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_gate, portMAX_DELAY);
    tts_buffer_reset();
    memset(s_timings_ms, 0, sizeof(s_timings_ms));
    s_last_error[0] = '\0';
    s_cancel_ms = -1;
    s_playback_wait_ms = 0;
    s_wait_metrics = false;
    s_reply_stage = "starting";
    s_accept_reply = false;
    s_awaiting_start = true;
    s_upload_enabled = true;
    snprintf(s_session_id, sizeof(s_session_id), "esp32-%u", ++s_turn);
    char request[128];
    snprintf(request, sizeof(request), "{\"event\":\"session.start\",\"session_id\":\"%s\"}",
             s_session_id);
    xSemaphoreGive(s_gate);
    const esp_err_t result = gateway_client_send_event(request);
    if (result != ESP_OK) {
        gateway_client_cancel();
    }
    return result;
}

activity_state_t gateway_client_activity_state(void) { return s_activity_state; }
bool gateway_client_activity_pending(void) { return s_activity_pending; }
const char *gateway_client_activity_theme(void) { return THEMES[s_activity_theme]; }
bool gateway_client_hint_ready(void)
{
    if (s_gate == NULL) return false;
    xSemaphoreTake(s_gate, portMAX_DELAY);
    const bool ready = hint_state_ready(&s_hint);
    xSemaphoreGive(s_gate);
    return ready;
}

void gateway_client_copy_hint(uint16_t text[81])
{
    text[0] = 0;
    if (s_gate == NULL) return;
    xSemaphoreTake(s_gate, portMAX_DELAY);
    memcpy(text, s_hint.text, sizeof(s_hint.text));
    xSemaphoreGive(s_gate);
}

void gateway_client_hint_dismiss(void)
{
    if (s_gate == NULL) return;
    xSemaphoreTake(s_gate, portMAX_DELAY);
    hint_state_clear(&s_hint);
    xSemaphoreGive(s_gate);
    notify(GATEWAY_ACTIVITY_CHANGED);
}
bool gateway_client_hints_enabled(void) { return s_hints_enabled; }

bool gateway_client_activity_capture_id(char id[33])
{
    if (s_gate == NULL) return false;
    xSemaphoreTake(s_gate, portMAX_DELAY);
    const bool active = s_connected && s_activity_state == ACTIVITY_ACTIVE &&
                        !s_activity_pending && s_activity_id[0] != '\0';
    if (active) memcpy(id, s_activity_id, sizeof(s_activity_id));
    xSemaphoreGive(s_gate);
    return active;
}

esp_err_t gateway_client_ambient_speech(const char *activity_id)
{
    gateway_client_hint_dismiss();
    char request[96];
    snprintf(request, sizeof(request), "{\"event\":\"ambient.speech\",\"activity_id\":\"%.32s\"}", activity_id);
    return gateway_client_send_event(request);
}

esp_err_t gateway_client_send_ambient(const char *activity_id, const void *data, size_t length)
{
    char current[33];
    if (data == NULL || length < 12800 || length > 16000 * 2 * 8 || length % 2 ||
        !gateway_client_activity_capture_id(current) || strcmp(current, activity_id) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *packet = heap_caps_malloc(length + 40, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet == NULL) return ESP_ERR_NO_MEM;
    memcpy(packet, "IBAMBI1\0", 8);
    memcpy(packet + 8, activity_id, 32);
    memcpy(packet + 40, data, length);
    const int sent = esp_websocket_client_send_bin(s_client, (const char *)packet,
                                                   length + 40, pdMS_TO_TICKS(1000));
    free(packet);
    return sent == (int)(length + 40) ? ESP_OK : ESP_FAIL;
}

esp_err_t gateway_client_hint_request(uint32_t silence_ms)
{
    xSemaphoreTake(s_gate, portMAX_DELAY);
    if (!s_connected || s_activity_state != ACTIVITY_ACTIVE || !s_hints_enabled || s_activity_pending ||
        s_hint.pending || hint_state_ready(&s_hint) || audio_player_busy()) {
        xSemaphoreGive(s_gate);
        return ESP_ERR_INVALID_STATE;
    }
    char request[96];
    snprintf(request, sizeof(request),
        "{\"event\":\"hint.request\",\"silence_ms\":%u,\"request_id\":%u}",
        (unsigned)silence_ms, (unsigned)hint_state_request(&s_hint));
    xSemaphoreGive(s_gate);
    const esp_err_t result = gateway_client_send_event(request);
    if (result != ESP_OK) gateway_client_hint_dismiss();
    return result;
}

int gateway_client_activity_stats(char *buffer, size_t size)
{
    const char *states[] = {"none", "active", "paused", "ended"};
    xSemaphoreTake(s_gate, portMAX_DELAY);
    const int length = snprintf(buffer, size,
        "{\"state\":\"%s\",\"pending\":%s,\"scene\":\"%s\",\"theme\":\"%s\","
        "\"history_turns\":%u,\"camera_enabled\":false,\"hints_enabled\":%s,\"hint_ready\":%s}\n",
        states[s_activity_state], s_activity_pending ? "true" : "false", s_activity_scene,
        THEMES[s_activity_theme], s_history_turns,
        (s_activity_state == ACTIVITY_ACTIVE || s_activity_state == ACTIVITY_PAUSED) && s_hints_enabled ? "true" : "false",
        hint_state_ready(&s_hint) ? "true" : "false");
    xSemaphoreGive(s_gate);
    return length;
}

esp_err_t gateway_client_activity(const char *action, const char *scene, const char *theme,
                                  bool hints_enabled)
{
    if (!s_connected || action == NULL || s_activity_pending) return ESP_ERR_INVALID_STATE;
    char request[224];
    if (strcmp(action, "start") == 0) {
        if (scene == NULL || theme == NULL) return ESP_ERR_INVALID_ARG;
        if (strcmp(scene, "social") != 0 && strcmp(scene, "date") != 0 && strcmp(scene, "meeting") != 0)
            return ESP_ERR_INVALID_ARG;
        bool valid = false;
        for (int i = 0; i < 4; ++i) valid |= strcmp(theme, THEMES[i]) == 0;
        if (!valid) return ESP_ERR_INVALID_ARG;
        snprintf(request, sizeof(request),
            "{\"event\":\"activity.start\",\"consent\":true,\"scene\":\"%s\",\"theme\":\"%s\",\"hints_enabled\":%s}",
            scene, theme, hints_enabled ? "true" : "false");
    } else if (strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0 || strcmp(action, "end") == 0) {
        snprintf(request, sizeof(request), "{\"event\":\"activity.%s\"}", action);
    } else if (strcmp(action, "hints_on") == 0 || strcmp(action, "hints_off") == 0) {
        snprintf(request, sizeof(request), "{\"event\":\"activity.hints.set\",\"enabled\":%s}",
                 strcmp(action, "hints_on") == 0 ? "true" : "false");
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    s_activity_pending = true;
    gateway_client_hint_dismiss();
    const esp_err_t result = gateway_client_send_event(request);
    if (result != ESP_OK) s_activity_pending = false;
    return result;
}

esp_err_t gateway_client_end_audio(void)
{
    if (!atomic_exchange(&s_upload_enabled, false)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_gate, portMAX_DELAY);
    s_wait_metrics = true;
    s_reply_started_ms = (unsigned)(esp_timer_get_time() / 1000);
    s_reply_stage = "asr";
    xSemaphoreGive(s_gate);
    return gateway_client_send_event("{\"event\":\"audio.end\"}");
}

esp_err_t gateway_client_send_event(const char *event_json)
{
    if (!s_connected || event_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = esp_websocket_client_send_text(s_client, event_json,
                                                    strlen(event_json), pdMS_TO_TICKS(100));
    return sent == strlen(event_json) ? ESP_OK : ESP_FAIL;
}

esp_err_t gateway_client_send_camera_snapshot(const char *capture_id, const uint8_t *data, size_t length,
                                              unsigned width, unsigned height)
{
    if (!s_connected || data == NULL || length == 0 || length > 128 * 1024 ||
        capture_id == NULL || strlen(capture_id) != 32) {
        return ESP_ERR_INVALID_STATE;
    }
    char metadata[192];
    const int metadata_length = snprintf(metadata, sizeof(metadata),
        "{\"event\":\"camera.snapshot\",\"size\":%u,\"width\":%u,\"height\":%u,\"capture_id\":\"%.32s\"}",
        (unsigned)length, width, height, capture_id);
    if (metadata_length <= 0 || metadata_length >= (int)sizeof(metadata) ||
        gateway_client_send_event(metadata) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t *packet = heap_caps_malloc(length + 40, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet == NULL) return ESP_ERR_NO_MEM;
    memcpy(packet, "IBJPEG1\0", 8);
    memcpy(packet + 8, capture_id, 32);
    memcpy(packet + 40, data, length);
    const int sent = esp_websocket_client_send_bin(s_client, (const char *)packet, length + 40, pdMS_TO_TICKS(1000));
    free(packet);
    return sent == (int)(length + 40) ? ESP_OK : ESP_FAIL;
}

esp_err_t gateway_client_send_audio(const void *data, size_t length)
{
    if (!s_connected || !s_upload_enabled || data == NULL || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = esp_websocket_client_send_bin(s_client, data, length,
                                                    pdMS_TO_TICKS(100));
    return sent == length ? ESP_OK : ESP_FAIL;
}
