#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned frees;
static void tracked_free(void *ptr)
{
    if (ptr != NULL) {
        ++frees;
    }
    free(ptr);
}

// Run the actual player deterministically; inject stop at the I2S write boundary.
#define free tracked_free
#include "../main/audio_player.c"
#undef free

static jmp_buf task_idle;
static playback_t queued;
static bool has_item;
static bool locked;
static unsigned starts, voice_writes, drains, stops;
static bool stop_on_write, fail_write, fail_start;
static audio_player_event_t last_event;

QueueHandle_t xQueueCreate(unsigned count, size_t size)
{
    assert(count == 1 && size == sizeof(playback_t));
    return &queued;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    (void)queue; (void)timeout;
    assert(!has_item);
    memcpy(&queued, item, sizeof(queued));
    has_item = true;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    (void)queue; (void)timeout;
    if (!has_item) {
        longjmp(task_idle, 1);
    }
    memcpy(item, &queued, sizeof(queued));
    has_item = false;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t gate, TickType_t timeout)
{
    (void)gate; (void)timeout;
    assert(!locked);
    locked = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t gate)
{
    (void)gate;
    assert(locked);
    locked = false;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t gate) { (void)gate; }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
int64_t esp_timer_get_time(void) { static int64_t now; return ++now; }
esp_err_t board_start_audio(void) { ++starts; return fail_start ? ESP_FAIL : ESP_OK; }
void board_stop_audio(void) { ++stops; }
esp_err_t board_play_pcm_mono(const int16_t *samples, size_t count)
{
    (void)samples;
    assert(!locked);
    if (count == 4800) {
        ++drains;
    } else {
        ++voice_writes;
        if (stop_on_write) {
            stop_on_write = false;
            audio_player_stop();
        }
    }
    return fail_write ? ESP_FAIL : ESP_OK;
}
static void callback(audio_player_event_t event) { last_event = event; }
static void run_player(void)
{
    if (setjmp(task_idle) == 0) {
        player_task(NULL);
    }
    assert(!locked && !audio_player_busy());
}
static void submit(void)
{
    uint8_t *pcm = calloc(12000, 2);
    assert(pcm != NULL);
    assert(audio_player_submit(pcm, 24000) == ESP_OK);
    assert(audio_player_busy());
}

int main(void)
{
    assert(audio_player_init(callback) == ESP_OK);
    assert(audio_player_submit(NULL, 2) == ESP_ERR_INVALID_ARG);
    uint8_t *caller_owned = malloc(2);
    assert(audio_player_submit(caller_owned, 1) == ESP_ERR_INVALID_ARG);
    submit();
    assert(audio_player_submit(caller_owned, 2) == ESP_ERR_INVALID_STATE);
    assert(frees == 0); // Failed submit must not take ownership.
    free(caller_owned);
    run_player();
    assert(frees == 1 && starts == 1 && voice_writes == 6 && drains == 1);
    assert(last_event == AUDIO_PLAYER_FINISHED && audio_player_bytes_written() == 24000);

    submit();
    audio_player_stop(); // Cancel while still queued; never enable the amplifier.
    run_player();
    assert(frees == 2 && starts == 1 && voice_writes == 6 && drains == 1);
    assert(last_event == AUDIO_PLAYER_INTERRUPTED);

    submit();
    stop_on_write = true;
    run_player();
    assert(frees == 3 && starts == 2 && voice_writes == 7 && drains == 2);
    assert(last_event == AUDIO_PLAYER_INTERRUPTED); // No later voice writes or restart.
    assert(audio_player_interrupt_us() > 0);

    submit(); // A new turn works after interruption.
    run_player();
    assert(frees == 4 && starts == 3 && last_event == AUDIO_PLAYER_FINISHED);

    fail_write = true;
    submit();
    run_player();
    assert(frees == 5 && audio_player_failures() == 1 && last_event == AUDIO_PLAYER_FAILED);
    fail_write = false;
    fail_start = true;
    submit();
    run_player();
    assert(frees == 6 && audio_player_failures() == 2 && last_event == AUDIO_PLAYER_FAILED);
    assert(stops >= 6);
    puts("PASS: ownership, queued cancel, mid-write cancel, DMA drain, next turn, failures");
}
