#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*usb_diag_capture_callback_t)(const char *capture_id, const uint8_t *data, size_t length,
                                            unsigned width, unsigned height);

esp_err_t usb_diag_start(usb_diag_capture_callback_t callback);
esp_err_t usb_diag_request_capture(const char *capture_id);
void usb_diag_cancel_capture(void);
