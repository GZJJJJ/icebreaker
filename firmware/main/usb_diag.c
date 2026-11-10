#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "usb_diag.h"

static const char *TAG = "usb_diag";

#define CAMERA_WIDTH 320
#define CAMERA_HEIGHT 240
#define CAMERA_FPS 25.0f
#define CAMERA_FRAME_SIZE (128 * 1024)

static atomic_bool s_frame_ready;
static atomic_bool s_stream_error;
static atomic_bool s_capture_requested;
static atomic_bool s_capture_busy;
static atomic_bool s_capture_cancelled;
static char s_capture_id[33];
static uint8_t *s_snapshot_buffer;
static size_t s_snapshot_length;
static unsigned s_snapshot_width;
static unsigned s_snapshot_height;
static usb_diag_capture_callback_t s_capture_callback;

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;
    if (atomic_load(&s_frame_ready) || atomic_load(&s_capture_cancelled)) return true;
    if (frame->data_len >= 4 && frame->data[0] == 0xff && frame->data[1] == 0xd8 &&
        frame->data[frame->data_len - 2] == 0xff && frame->data[frame->data_len - 1] == 0xd9) {
        if (s_snapshot_buffer == NULL || frame->data_len > CAMERA_FRAME_SIZE) {
            ESP_LOGE(TAG, "UVC snapshot is too large: %u bytes", (unsigned)frame->data_len);
        } else {
            memcpy(s_snapshot_buffer, frame->data, frame->data_len);
            s_snapshot_length = frame->data_len;
            s_snapshot_width = frame->vs_format.h_res;
            s_snapshot_height = frame->vs_format.v_res;
            ESP_LOGI(TAG, "UVC snapshot JPEG ready: %u bytes (%ux%u)",
                     (unsigned)frame->data_len, frame->vs_format.h_res, frame->vs_format.v_res);
            atomic_store(&s_frame_ready, true);
        }
    } else {
        ESP_LOGW(TAG, "Ignoring incomplete UVC frame: %u bytes", (unsigned)frame->data_len);
    }
    return true;
}

static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == UVC_HOST_TRANSFER_ERROR) {
        ESP_LOGE(TAG, "UVC transfer error: %s", esp_err_to_name(event->transfer_error.error));
        atomic_store(&s_stream_error, true);
    } else if (event->type == UVC_HOST_FRAME_BUFFER_OVERFLOW) {
        ESP_LOGE(TAG, "UVC frame buffer overflow");
        atomic_store(&s_stream_error, true);
    } else if (event->type == UVC_HOST_FRAME_BUFFER_UNDERFLOW) {
        ESP_LOGE(TAG, "UVC frame buffer underflow");
        atomic_store(&s_stream_error, true);
    } else if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "UVC camera disconnected");
        atomic_store(&s_stream_error, true);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void snapshot_task(void *arg)
{
    (void)arg;
    const uvc_host_stream_config_t stream_config = {
        .event_cb = stream_callback,
        .frame_cb = frame_callback,
        .user_ctx = NULL,
        .usb = {
            .dev_addr = UVC_HOST_ANY_DEV_ADDR,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = 0,
        },
        .vs_format = {
            .h_res = CAMERA_WIDTH,
            .v_res = CAMERA_HEIGHT,
            .fps = CAMERA_FPS,
            .format = UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = 2,
            .frame_size = CAMERA_FRAME_SIZE,
            .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .number_of_urbs = 4,
            .urb_size = 10 * 1024,
        },
    };

    while (true) {
        if (!atomic_exchange(&s_capture_requested, false)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (atomic_load(&s_capture_cancelled)) {
            atomic_store(&s_capture_busy, false);
            continue;
        }
        s_snapshot_buffer = heap_caps_malloc(CAMERA_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_snapshot_buffer == NULL) {
            s_capture_callback(s_capture_id, NULL, 0, 0, 0);
            atomic_store(&s_capture_busy, false);
            continue;
        }
        uvc_host_stream_hdl_t stream = NULL;
        atomic_store(&s_frame_ready, false);
        atomic_store(&s_stream_error, false);
        const esp_err_t open_result = uvc_host_stream_open(&stream_config, pdMS_TO_TICKS(5000), &stream);
        if (open_result != ESP_OK) {
            ESP_LOGE(TAG, "UVC stream open failed: %s", esp_err_to_name(open_result));
            free(s_snapshot_buffer);
            s_snapshot_buffer = NULL;
            s_capture_callback(s_capture_id, NULL, 0, 0, 0);
            atomic_store(&s_capture_busy, false);
            continue;
        }

        uvc_host_stream_format_t selected;
        if (uvc_host_stream_format_get(stream, &selected) == ESP_OK) {
            ESP_LOGI(TAG, "UVC stream selected: %ux%u %.1ffps format=%d",
                     selected.h_res, selected.v_res, selected.fps, selected.format);
        }
        const esp_err_t start_result = atomic_load(&s_capture_cancelled) ? ESP_FAIL : uvc_host_stream_start(stream);
        if (start_result != ESP_OK) {
            ESP_LOGE(TAG, "UVC stream start failed: %s", esp_err_to_name(start_result));
            uvc_host_stream_close(stream);
            free(s_snapshot_buffer);
            s_snapshot_buffer = NULL;
            s_capture_callback(s_capture_id, NULL, 0, 0, 0);
            atomic_store(&s_capture_busy, false);
            continue;
        }

        const int64_t deadline = esp_timer_get_time() + 10000000;
        while (!atomic_load(&s_frame_ready) && !atomic_load(&s_stream_error) &&
               !atomic_load(&s_capture_cancelled) &&
               esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        uvc_host_stream_stop(stream);
        uvc_host_stream_close(stream);
        if (atomic_load(&s_frame_ready) && !atomic_load(&s_capture_cancelled)) {
            ESP_LOGI(TAG, "UVC snapshot validation: JPEG SOI/EOI present");
            if (s_capture_callback != NULL) {
                s_capture_callback(s_capture_id, s_snapshot_buffer, s_snapshot_length,
                                   s_snapshot_width, s_snapshot_height);
            }
        } else {
            ESP_LOGE(TAG, "UVC snapshot failed before a complete JPEG frame");
            s_capture_callback(s_capture_id, NULL, 0, 0, 0);
        }
        free(s_snapshot_buffer);
        s_snapshot_buffer = NULL;
        atomic_store(&s_capture_busy, false);
    }
}

esp_err_t usb_diag_start(usb_diag_capture_callback_t callback)
{
    if (callback == NULL) return ESP_ERR_INVALID_ARG;
    s_capture_callback = callback;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t result = usb_host_install(&host_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(result));
        return result;
    }
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 10,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = NULL,
        .user_ctx = NULL,
    };
    result = uvc_host_install(&driver_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UVC driver install failed: %s", esp_err_to_name(result));
        return result;
    }
    if (xTaskCreate(snapshot_task, "uvc_snapshot", 8192, NULL, 9, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB host UVC snapshot ready (waiting for explicit capture)");
    return ESP_OK;
}

esp_err_t usb_diag_request_capture(const char *capture_id)
{
    if (capture_id == NULL || strlen(capture_id) != 32) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 32; ++i) {
        if (!((capture_id[i] >= '0' && capture_id[i] <= '9') ||
              (capture_id[i] >= 'a' && capture_id[i] <= 'f'))) return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_capture_busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_capture_id, capture_id, sizeof(s_capture_id));
    atomic_store(&s_capture_cancelled, false);
    atomic_store(&s_capture_requested, true);
    return ESP_OK;
}

void usb_diag_cancel_capture(void)
{
    atomic_store(&s_capture_cancelled, true);
}
