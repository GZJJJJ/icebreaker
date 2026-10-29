#include "audio_player.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    uint8_t *pcm;
    size_t length;
} playback_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_gate;
static void (*s_callback)(audio_player_event_t event);
static atomic_bool s_busy;
static atomic_bool s_cancelled;
static atomic_uint s_bytes_written;
static atomic_uint s_failures;
static atomic_uint s_interrupt_us;

static void player_task(void *arg)
{
    playback_t playback;
    while (xQueueReceive(s_queue, &playback, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(s_gate, portMAX_DELAY);
        esp_err_t result = ESP_OK;
        bool started = false;
        if (!s_cancelled) {
            result = board_start_audio();
            started = result == ESP_OK;
            if (result == ESP_OK && s_callback != NULL) {
                s_callback(AUDIO_PLAYER_STARTED);
            }
        }
        xSemaphoreGive(s_gate);

        const int16_t *samples = (const int16_t *)playback.pcm;
        const size_t count = playback.length / sizeof(int16_t);
        for (size_t offset = 0; offset < count && result == ESP_OK && !s_cancelled; offset += 2048) {
            const size_t chunk = count - offset > 2048 ? 2048 : count - offset;
            // Never re-enable the codec or amplifier here; stop must stay effective.
            result = board_play_pcm_mono(samples + offset, chunk);
            if (result == ESP_OK) {
                s_bytes_written += chunk * sizeof(int16_t);
            }
        }
        // Drain DMA, also after cancellation (while muted), before accepting new PCM.
        static const int16_t silence[4800] = {0};
        if (started && result == ESP_OK) {
            result = board_play_pcm_mono(silence, 4800);
        }

        xSemaphoreTake(s_gate, portMAX_DELAY);
        board_stop_audio();
        free(playback.pcm);
        if (result != ESP_OK) {
            s_failures++;
        }
        if (s_callback != NULL) {
            s_callback(s_cancelled ? AUDIO_PLAYER_INTERRUPTED :
                       result == ESP_OK ? AUDIO_PLAYER_FINISHED : AUDIO_PLAYER_FAILED);
        }
        s_busy = false;
        xSemaphoreGive(s_gate);
    }
}

esp_err_t audio_player_init(void (*callback)(audio_player_event_t event))
{
    s_callback = callback;
    s_gate = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(playback_t));
    if (s_gate == NULL || s_queue == NULL) {
        if (s_gate != NULL) {
            vSemaphoreDelete(s_gate);
        }
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(player_task, "audio_player", 8192, NULL, 6, NULL) != pdPASS) {
        vSemaphoreDelete(s_gate);
        vQueueDelete(s_queue);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_player_submit(uint8_t *pcm, size_t length)
{
    if (pcm == NULL || length == 0 || length % sizeof(int16_t) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_gate, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!s_busy) {
        const playback_t playback = {.pcm = pcm, .length = length};
        s_cancelled = false;
        s_busy = true;
        if (xQueueSend(s_queue, &playback, 0) == pdTRUE) {
            result = ESP_OK;
        } else {
            s_busy = false;
        }
    }
    xSemaphoreGive(s_gate);
    return result;
}

void audio_player_stop(void)
{
    const int64_t started = esp_timer_get_time();
    xSemaphoreTake(s_gate, portMAX_DELAY);
    s_cancelled = true;
    board_stop_audio();
    xSemaphoreGive(s_gate);
    s_interrupt_us = (unsigned)(esp_timer_get_time() - started);
}

bool audio_player_busy(void) { return s_busy; }
uint32_t audio_player_bytes_written(void) { return s_bytes_written; }
uint32_t audio_player_failures(void) { return s_failures; }
uint32_t audio_player_interrupt_us(void) { return s_interrupt_us; }
