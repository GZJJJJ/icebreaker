#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board.h"
#include "audio_player.h"
#include "gateway_client.h"

static const char *TAG = "icebreaker";

typedef enum {
    APP_STATE_BOOT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_IDLE,
    APP_STATE_LISTENING,
    APP_STATE_THINKING,
    APP_STATE_SPEAKING,
    APP_STATE_INTERRUPTED,
    APP_STATE_ERROR,
} app_state_t;

static atomic_int s_state = APP_STATE_BOOT;
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_button_events;
static httpd_handle_t s_http_server;
static atomic_bool s_audio_stopped;
static atomic_bool s_capture_requested;
static atomic_bool s_capture_active;
static atomic_bool s_capture_cancelled;
static atomic_uint s_button_presses;

static const char *state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_BOOT: return "boot";
    case APP_STATE_WIFI_CONNECTING: return "wifi_connecting";
    case APP_STATE_IDLE: return "idle";
    case APP_STATE_LISTENING: return "listening";
    case APP_STATE_THINKING: return "thinking";
    case APP_STATE_SPEAKING: return "speaking";
    case APP_STATE_INTERRUPTED: return "interrupted";
    case APP_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

static void set_state(app_state_t state)
{
    const app_state_t previous = atomic_exchange(&s_state, state);
    if (previous == state) {
        return;
    }

    ESP_LOGI(TAG, "state: %s -> %s", state_name(previous), state_name(state));
    board_display_status(state_name(state));
}

static void player_event(audio_player_event_t event)
{
    if (event == AUDIO_PLAYER_STARTED) {
        gateway_client_playback_started();
        s_audio_stopped = false;
        set_state(APP_STATE_SPEAKING);
    } else if (s_state == APP_STATE_SPEAKING) {
        set_state(event == AUDIO_PLAYER_FINISHED ? APP_STATE_IDLE :
                  event == AUDIO_PLAYER_INTERRUPTED ? APP_STATE_INTERRUPTED : APP_STATE_ERROR);
    }
}

static void gateway_event(gateway_event_t event)
{
    if (event == GATEWAY_READY) {
        if (!s_capture_active && !audio_player_busy()) {
            set_state(APP_STATE_IDLE);
        }
    } else if (event == GATEWAY_THINKING) {
        if (!s_capture_requested && !s_capture_cancelled) {
            set_state(APP_STATE_THINKING);
        }
    } else if (event == GATEWAY_IDLE) {
        if (!audio_player_busy() && !s_capture_requested) {
            set_state(APP_STATE_IDLE);
        }
    } else {
        s_capture_cancelled = true;
        s_capture_requested = false;
        set_state(event == GATEWAY_INTERRUPTED ? APP_STATE_INTERRUPTED : APP_STATE_ERROR);
    }
}

static void stop_audio(void)
{
    const int64_t requested_at = esp_timer_get_time();

    s_capture_cancelled = true;
    s_capture_requested = false;
    gateway_client_cancel();
    s_audio_stopped = true;
    set_state(APP_STATE_INTERRUPTED);
    gateway_client_send_event("{\"event\":\"session.interrupt\"}");

    ESP_LOGI(TAG, "audio stopped locally at %lld us", requested_at);
}

static void capture_task(void *arg)
{
    enum { SAMPLE_RATE = 24000, CHUNK_MS = 100, TARGET_RATE = 16000 };
    const size_t frames = SAMPLE_RATE * CHUNK_MS / 1000;
    const size_t out_frames = frames * TARGET_RATE / SAMPLE_RATE;
    int16_t *pcm = calloc(frames, sizeof(int16_t));
    int16_t *downsampled = calloc(out_frames, sizeof(int16_t));
    if (pcm == NULL || downsampled == NULL) {
        free(pcm);
        free(downsampled);
        vTaskDelete(NULL);
        return;
    }

    size_t captured_frames = 0;
    while (true) {
        if (!s_capture_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_capture_requested) {
            // Only this task sends audio.end, after the final read/send has returned.
            if (!s_capture_cancelled) {
                set_state(APP_STATE_THINKING);
                if (gateway_client_end_audio() != ESP_OK) {
                    gateway_client_cancel();
                    set_state(APP_STATE_ERROR);
                }
            }
            captured_frames = 0;
            s_capture_active = false;
            continue;
        }
        if (board_read_audio(pcm, frames) != ESP_OK) {
            if (s_capture_cancelled) {
                continue;
            }
            ESP_LOGW(TAG, "audio capture failed");
            stop_audio();
            set_state(APP_STATE_ERROR);
            continue;
        }
        if (s_capture_cancelled) {
            continue;
        }
        // Gateway and SenseVoice run at 16 kHz; the board codec runs at 24 kHz.
        for (size_t i = 0; i < out_frames; ++i) {
            const size_t src = (i * SAMPLE_RATE) / TARGET_RATE;
            downsampled[i] = pcm[src < frames ? src : frames - 1];
        }
        if (gateway_client_send_audio(downsampled, out_frames * sizeof(int16_t)) != ESP_OK) {
            if (s_capture_cancelled) {
                continue;
            }
            ESP_LOGW(TAG, "audio send failed");
            stop_audio();
            set_state(APP_STATE_ERROR);
            continue;
        }
        captured_frames += frames;
        if (captured_frames >= SAMPLE_RATE * 30) {
            s_capture_requested = false;
        }
        // I2S read already waits for one frame; do not add another 100 ms delay.
    }
}

static void IRAM_ATTR button_isr(void *arg)
{
    const uint32_t event = 1;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(s_button_events, &event, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    uint32_t event;
    TickType_t last_trigger = 0;
    while (xQueueReceive(s_button_events, &event, portMAX_DELAY) == pdTRUE) {
        (void)event;
        vTaskDelay(pdMS_TO_TICKS(40));
        while (xQueueReceive(s_button_events, &event, 0) == pdTRUE) {
        }
        const TickType_t now = xTaskGetTickCount();
        if (last_trigger != 0 && (now - last_trigger) < pdMS_TO_TICKS(500)) {
            continue;
        }
        last_trigger = now;
        s_button_presses++;
        ESP_LOGI(TAG, "button pressed: state=%s capture=%s",
                 state_name(s_state), s_capture_requested ? "on" : "off");

        if (audio_player_busy() || s_state == APP_STATE_SPEAKING || s_state == APP_STATE_THINKING) {
            stop_audio();
            continue;
        }
        if (s_capture_requested) {
            s_capture_requested = false;
            ESP_LOGI(TAG, "capture ended");
            set_state(APP_STATE_THINKING);
        } else if (!s_capture_active) {
            s_capture_cancelled = false;
            if (gateway_client_begin_turn() != ESP_OK) {
                ESP_LOGW(TAG, "Gateway is not connected; cannot start capture");
                continue;
            }
            s_capture_requested = true;
            s_capture_active = true;
            ESP_LOGI(TAG, "capture started");
            set_state(APP_STATE_LISTENING);
        }
    }
}

static esp_err_t button_init(void)
{
    s_button_events = xQueueCreate(4, sizeof(uint32_t));
    if (s_button_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ICEBREAKER_BUTTON_GPIO >= 0
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_ICEBREAKER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ICEBREAKER_BUTTON_GPIO, button_isr, NULL));
    ESP_LOGI(TAG, "interrupt button enabled on GPIO %d", CONFIG_ICEBREAKER_BUTTON_GPIO);
#else
    ESP_LOGW(TAG, "interrupt button disabled; set ICEBREAKER_BUTTON_GPIO after pin confirmation");
#endif

    xTaskCreate(button_task, "button_task", 3072, NULL, 10, NULL);
    xTaskCreate(capture_task, "capture_task", 4096, NULL, 8, NULL);
    return ESP_OK;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    char response[320];
    const int length = snprintf(response, sizeof(response),
                                "{\"ok\":true,\"state\":\"%s\",\"audio_stopped\":%s,"
                                "\"capture_active\":%s,\"capture_requested\":%s,"
                                "\"button_presses\":%u,\"uptime_ms\":%lld}\n",
                                state_name(s_state), s_audio_stopped ? "true" : "false",
                                s_capture_active ? "true" : "false",
                                s_capture_requested ? "true" : "false",
                                (unsigned)s_button_presses, esp_timer_get_time() / 1000);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t button_handler(httpd_req_t *request)
{
    // Trusted-LAN diagnostic: exercise exactly the same queue/task as GPIO K0.
    const uint32_t event = 1;
    if (xQueueSend(s_button_events, &event, 0) != pdTRUE) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Button queue full");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true,\"queued\":true}\n");
}

static esp_err_t interrupt_handler(httpd_req_t *request)
{
    stop_audio();
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true,\"state\":\"interrupted\"}\n");
}

static esp_err_t tone_handler(httpd_req_t *request)
{
    if (s_capture_active || s_state == APP_STATE_THINKING || audio_player_busy()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Device is busy");
    }
    enum { FRAMES = 24000 * 3 };
    int16_t *pcm = malloc(FRAMES * sizeof(int16_t));
    if (pcm == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No PCM buffer");
    }
    for (size_t i = 0; i < FRAMES; ++i) {
        pcm[i] = i % 24 < 12 ? 8000 : -8000;
    }
    if (audio_player_submit((uint8_t *)pcm, FRAMES * sizeof(int16_t)) != ESP_OK) {
        free(pcm);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Player busy");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true,\"queued\":true,\"tone\":\"1000Hz 3000ms\"}\n");
}

static esp_err_t stats_handler(httpd_req_t *request)
{
    char response[1024];
    const int length = gateway_client_stats(response, sizeof(response));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t play_handler(httpd_req_t *request)
{
    // Development-only input: raw PCM16LE, 24 kHz, mono, at most five seconds.
    if (request->content_len <= 0 || request->content_len > 24000 * 2 * 5 ||
        request->content_len % sizeof(int16_t) != 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid PCM length");
    }
    if (s_capture_active || s_state == APP_STATE_THINKING || audio_player_busy()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Device is busy");
    }
    int16_t *pcm = malloc(request->content_len);
    if (pcm == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No PCM buffer");
    }
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, (char *)pcm + received,
                                        request->content_len - received);
        if (count <= 0) {
            free(pcm);
            return ESP_FAIL;
        }
        received += count;
    }
    const esp_err_t result = audio_player_submit((uint8_t *)pcm, received);
    if (result != ESP_OK) {
        free(pcm);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Player busy");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true,\"queued\":true,\"sample_rate\":24000,\"channels\":1}\n");
}

static esp_err_t http_server_start(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_ICEBREAKER_HTTP_PORT;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "start HTTP server failed");

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    const httpd_uri_t interrupt_uri = {
        .uri = "/api/interrupt",
        .method = HTTP_POST,
        .handler = interrupt_handler,
    };
    const httpd_uri_t button_uri = {
        .uri = "/api/button",
        .method = HTTP_POST,
        .handler = button_handler,
    };
    const httpd_uri_t tone_uri = {
        .uri = "/api/tone",
        .method = HTTP_POST,
        .handler = tone_handler,
    };
    const httpd_uri_t stats_uri = {
        .uri = "/api/stats",
        .method = HTTP_GET,
        .handler = stats_handler,
    };
    const httpd_uri_t play_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = play_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &health_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &interrupt_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &button_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &tone_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &stats_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &play_uri));
    ESP_LOGI(TAG, "HTTP server listening on port %d", CONFIG_ICEBREAKER_HTTP_PORT);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_capture_cancelled = true;
        s_capture_requested = false;
        gateway_client_cancel();
        xEventGroupClearBits(s_wifi_events, BIT0);
        set_state(APP_STATE_WIFI_CONNECTING);
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, BIT0);
        set_state(APP_STATE_IDLE);
        http_server_start();
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static esp_err_t wifi_init(void)
{
    if (CONFIG_ICEBREAKER_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi is not configured; set ICEBREAKER_WIFI_SSID with menuconfig");
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_ICEBREAKER_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_ICEBREAKER_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

#if CONFIG_ICEBREAKER_WIFI_AUTH_ENTERPRISE
    ESP_ERROR_CHECK(esp_eap_client_set_username(
        (const unsigned char *)CONFIG_ICEBREAKER_WIFI_USERNAME,
        strlen(CONFIG_ICEBREAKER_WIFI_USERNAME)));
    ESP_ERROR_CHECK(esp_eap_client_set_password(
        (const unsigned char *)CONFIG_ICEBREAKER_WIFI_PASSWORD,
        strlen(CONFIG_ICEBREAKER_WIFI_PASSWORD)));
    if (CONFIG_ICEBREAKER_WIFI_IDENTITY[0] != '\0') {
        ESP_ERROR_CHECK(esp_eap_client_set_identity(
            (const unsigned char *)CONFIG_ICEBREAKER_WIFI_IDENTITY,
            strlen(CONFIG_ICEBREAKER_WIFI_IDENTITY)));
    }
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    set_state(APP_STATE_WIFI_CONNECTING);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    ESP_ERROR_CHECK(board_init());
    board_display_status("boot");
    board_mic_probe();
    ESP_ERROR_CHECK(audio_player_init(player_event));
    ESP_ERROR_CHECK(button_init());

    set_state(APP_STATE_BOOT);
    ESP_ERROR_CHECK(wifi_init());
    if (CONFIG_ICEBREAKER_WIFI_SSID[0] == '\0') {
        set_state(APP_STATE_IDLE);
    }
    if (CONFIG_ICEBREAKER_WIFI_SSID[0] != '\0') {
        ESP_ERROR_CHECK(gateway_client_start(gateway_event));
    }

    ESP_LOGI(TAG, "standalone ESP-IDF firmware ready");
    ESP_LOGI(TAG, "no OpenVela dependency is present in this build");
}
