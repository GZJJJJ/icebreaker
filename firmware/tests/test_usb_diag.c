#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define pdMS_TO_TICKS(ms) (ms)
#define pdFALSE 0
#define tskNO_AFFINITY (-1)
static void vTaskDelay(uint32_t ticks);
static unsigned allocations;
static void tracked_free(void *pointer) { if (pointer) --allocations; free(pointer); }
#define free tracked_free
#include "../main/usb_diag.c"
#undef free

enum { VALID, OPEN_FAIL, START_FAIL, TIMEOUT, CANCEL_OPEN, CANCEL_STREAM, TRANSFER_ERROR };
static int mode;
static jmp_buf idle;
static unsigned opens, starts, closes, callbacks, successes, frames;
static bool streaming;
static int64_t now;
static const char *id = "0123456789abcdef0123456789abcdef";

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    void *pointer = malloc(size);
    if (pointer) ++allocations;
    return pointer;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
esp_err_t usb_host_install(const usb_host_config_t *config) { (void)config; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(uint32_t timeout, uint32_t *flags)
{ (void)timeout; *flags = 0; return ESP_OK; }
void usb_host_device_free_all(void) {}
esp_err_t uvc_host_install(const uvc_host_driver_config_t *config) { (void)config; return ESP_OK; }
esp_err_t uvc_host_stream_open(const uvc_host_stream_config_t *config, uint32_t timeout, uvc_host_stream_hdl_t *stream)
{
    assert(config->vs_format.h_res == 320 && timeout == 5000);
    ++opens;
    if (mode == CANCEL_OPEN) usb_diag_cancel_capture();
    *stream = (void *)1;
    return mode == OPEN_FAIL ? ESP_FAIL : ESP_OK;
}
esp_err_t uvc_host_stream_format_get(uvc_host_stream_hdl_t stream, uvc_host_stream_format_t *format)
{ (void)stream; *format = (uvc_host_stream_format_t){320, 240, 25, 1}; return ESP_OK; }
esp_err_t uvc_host_stream_start(uvc_host_stream_hdl_t stream)
{ (void)stream; ++starts; streaming = mode != START_FAIL; frames = 0; return streaming ? ESP_OK : ESP_FAIL; }
esp_err_t uvc_host_stream_stop(uvc_host_stream_hdl_t stream) { (void)stream; streaming = false; return ESP_OK; }
esp_err_t uvc_host_stream_close(uvc_host_stream_hdl_t stream) { (void)stream; ++closes; return ESP_OK; }
int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(uint32_t ticks)
{
    now += ticks * 1000;
    if (!s_capture_busy) longjmp(idle, 1);
    if (!streaming) return;
    if (mode == CANCEL_STREAM) { usb_diag_cancel_capture(); return; }
    if (mode == TRANSFER_ERROR) {
        const uvc_host_stream_event_data_t error = {.type = UVC_HOST_TRANSFER_ERROR};
        stream_callback(&error, NULL);
    }
    if (mode != VALID) return;
    uint8_t bytes[] = {0xff, 0xd8, 0, 0, 0xff, 0xd9};
    uvc_host_frame_t frame = {.data = bytes, .data_len = ++frames == 1 ? 3 : sizeof(bytes),
                              .vs_format = {320, 240, 25, 1}};
    assert(frame_callback(&frame, NULL));
    memset(bytes, 0, sizeof(bytes)); // Callback must copy, not retain the driver's buffer.
}
static void callback(const char *capture_id, const uint8_t *data, size_t length, unsigned width, unsigned height)
{
    ++callbacks;
    assert(strcmp(capture_id, id) == 0 && !streaming);
    if (data) {
        ++successes;
        assert(length == 6 && width == 320 && height == 240);
        assert(data[0] == 0xff && data[1] == 0xd8 && data[5] == 0xd9);
    }
}
static void run_task(void)
{
    if (setjmp(idle) == 0) snapshot_task(NULL);
    assert(!s_capture_busy && !streaming && allocations == 0 && s_snapshot_buffer == NULL);
}
int main(void)
{
    (void)TAG;
    assert(usb_diag_start(callback) == ESP_OK);
    run_task();
    assert(opens == 0 && starts == 0 && callbacks == 0); // Boot never captures.
    assert(usb_diag_request_capture("invalid") == ESP_ERR_INVALID_ARG);
    assert(usb_diag_request_capture(id) == ESP_OK);
    assert(usb_diag_request_capture(id) == ESP_ERR_INVALID_STATE);
    usb_diag_cancel_capture();
    run_task();
    assert(opens == 0 && callbacks == 0); // Cancel queued request before USB opens.
    for (mode = VALID; mode <= TRANSFER_ERROR; ++mode) {
        assert(usb_diag_request_capture(id) == ESP_OK);
        run_task();
    }
    assert(opens == 7 && starts == 5 && closes == 6 && callbacks == 7 && successes == 1);
    mode = VALID;
    assert(usb_diag_request_capture(id) == ESP_OK);
    run_task();
    assert(successes == 2);
    puts("PASS: no boot capture, consent ID, busy, queued/active cancel, copy ownership, timeout, errors, repeat");
}
