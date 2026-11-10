#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { bool skip_phy_setup; bool root_port_unpowered; int intr_flags; } usb_host_config_t;
#define ESP_INTR_FLAG_LEVEL1 1
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 1
esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_lib_handle_events(uint32_t timeout, uint32_t *flags);
void usb_host_device_free_all(void);
