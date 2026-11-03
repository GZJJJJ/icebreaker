#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "usb_diag.h"

static const char *TAG = "usb_diag";
static usb_host_client_handle_t s_client;

static uint16_t descriptor_u16(const uint8_t *descriptor, size_t offset)
{
    return descriptor[offset] | ((uint16_t)descriptor[offset + 1] << 8);
}

static void usb_host_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB host has no clients");
            break;
        }
    }
    vTaskDelete(NULL);
}

static void log_configuration(const usb_config_desc_t *config)
{
    const uint8_t *descriptor = (const uint8_t *)config;
    size_t offset = 0;
    while (offset + 2 <= config->wTotalLength) {
        const uint8_t length = descriptor[offset];
        if (length < 2 || offset + length > config->wTotalLength) break;
        if (descriptor[offset + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= 9) {
            ESP_LOGI(TAG, "interface=%u class=0x%02x subclass=0x%02x protocol=0x%02x",
                     descriptor[offset + 2], descriptor[offset + 5],
                     descriptor[offset + 6], descriptor[offset + 7]);
        }
        if (descriptor[offset + 1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && length >= 7) {
            ESP_LOGI(TAG, "endpoint=0x%02x transfer=%u mps=%u",
                     descriptor[offset + 2], descriptor[offset + 3] & 0x03,
                     descriptor_u16(descriptor, offset + 4) & 0x07ff);
        }
        if (descriptor[offset + 1] == 0x24 && length >= 4) {
            const uint8_t subtype = descriptor[offset + 2];
            if (subtype == 0x06 && length >= 6) {
                ESP_LOGI(TAG, "UVC MJPEG format=%u frames=%u",
                         descriptor[offset + 3], descriptor[offset + 4]);
            } else if (subtype == 0x07 && length >= 9) {
                ESP_LOGI(TAG, "UVC MJPEG frame=%u %ux%u",
                         descriptor[offset + 3], descriptor_u16(descriptor, offset + 5),
                         descriptor_u16(descriptor, offset + 7));
            }
        }
        offset += length;
    }
}

static void usb_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    usb_host_client_handle_t client = s_client;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "USB device removed");
        return;
    }
    if (event_msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV) return;

    usb_device_handle_t device;
    const esp_err_t open_result = usb_host_device_open(client, event_msg->new_dev.address, &device);
    if (open_result != ESP_OK) {
        ESP_LOGE(TAG, "USB device open failed: %s", esp_err_to_name(open_result));
        return;
    }

    usb_device_info_t info;
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_device_info(device, &info) == ESP_OK &&
        usb_host_get_device_descriptor(device, &device_desc) == ESP_OK) {
        ESP_LOGI(TAG, "USB device addr=%u speed=%s VID=0x%04x PID=0x%04x class=0x%02x configs=%u",
                 info.dev_addr, info.speed == USB_SPEED_FULL ? "full" : "low",
                 device_desc->idVendor, device_desc->idProduct,
                 device_desc->bDeviceClass, device_desc->bNumConfigurations);
    }
    if (usb_host_get_active_config_descriptor(device, &config_desc) == ESP_OK) {
        ESP_LOGI(TAG, "USB configuration length=%u interfaces=%u power=%umA",
                 config_desc->wTotalLength, config_desc->bNumInterfaces,
                 config_desc->bMaxPower * 2);
        log_configuration(config_desc);
    }
    ESP_ERROR_CHECK(usb_host_device_close(client, device));
}

static void usb_client_task(void *arg)
{
    usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 4,
        .async = {
            .client_event_callback = usb_client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&config, &s_client));
    ESP_LOGI(TAG, "USB host client ready; waiting for camera");
    while (true) {
        ESP_ERROR_CHECK(usb_host_client_handle_events(s_client, portMAX_DELAY));
    }
}

esp_err_t usb_diag_start(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };
    const esp_err_t install_result = usb_host_install(&host_config);
    if (install_result != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(install_result));
        return install_result;
    }
    if (xTaskCreate(usb_host_lib_task, "usb_host_lib", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(usb_client_task, "usb_client", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB host diagnostic started");
    return ESP_OK;
}
