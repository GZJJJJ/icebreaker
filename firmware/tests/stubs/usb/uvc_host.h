#pragma once
#include <stddef.h>
#include "usb/usb_host.h"
#define UVC_HOST_ANY_DEV_ADDR 0
#define UVC_HOST_ANY_VID 0
#define UVC_HOST_ANY_PID 0
#define UVC_VS_FORMAT_MJPEG 1
enum { UVC_HOST_TRANSFER_ERROR, UVC_HOST_FRAME_BUFFER_OVERFLOW,
       UVC_HOST_FRAME_BUFFER_UNDERFLOW, UVC_HOST_DEVICE_DISCONNECTED };
typedef void *uvc_host_stream_hdl_t;
typedef struct { unsigned h_res, v_res; float fps; int format; } uvc_host_stream_format_t;
typedef struct { uint8_t *data; size_t data_len; uvc_host_stream_format_t vs_format; } uvc_host_frame_t;
typedef struct { int type; struct { esp_err_t error; } transfer_error; } uvc_host_stream_event_data_t;
typedef struct {
    void (*event_cb)(const uvc_host_stream_event_data_t *, void *);
    bool (*frame_cb)(const uvc_host_frame_t *, void *);
    void *user_ctx;
    struct { int dev_addr, vid, pid, uvc_stream_index; } usb;
    uvc_host_stream_format_t vs_format;
    struct { int number_of_frame_buffers; size_t frame_size; unsigned frame_heap_caps;
             int number_of_urbs; size_t urb_size; } advanced;
} uvc_host_stream_config_t;
typedef struct {
    unsigned driver_task_stack_size, driver_task_priority;
    int xCoreID;
    bool create_background_task;
    void *event_cb, *user_ctx;
} uvc_host_driver_config_t;
esp_err_t uvc_host_install(const uvc_host_driver_config_t *config);
esp_err_t uvc_host_stream_open(const uvc_host_stream_config_t *config, uint32_t timeout, uvc_host_stream_hdl_t *stream);
esp_err_t uvc_host_stream_format_get(uvc_host_stream_hdl_t stream, uvc_host_stream_format_t *format);
esp_err_t uvc_host_stream_start(uvc_host_stream_hdl_t stream);
esp_err_t uvc_host_stream_stop(uvc_host_stream_hdl_t stream);
esp_err_t uvc_host_stream_close(uvc_host_stream_hdl_t stream);
