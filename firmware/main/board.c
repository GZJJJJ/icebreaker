#include "board.h"
#include "board_audio_control.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board";

#define XL9555_ADDR 0x20
#define XL9555_REG_INPUT_0 0x00
#define XL9555_REG_OUTPUT_0 0x02
#define XL9555_REG_CONFIG_0 0x06

#define BOARD_I2C_SDA GPIO_NUM_48
#define BOARD_I2C_SCL GPIO_NUM_45

#define LCD_CS GPIO_NUM_1
#define LCD_DC GPIO_NUM_2
#define LCD_WR GPIO_NUM_42
#define LCD_RD GPIO_NUM_41
#define LCD_D0 GPIO_NUM_40
#define LCD_D1 GPIO_NUM_39
#define LCD_D2 GPIO_NUM_38
#define LCD_D3 GPIO_NUM_12
#define LCD_D4 GPIO_NUM_11
#define LCD_D5 GPIO_NUM_10
#define LCD_D6 GPIO_NUM_9
#define LCD_D7 GPIO_NUM_46

#define AUDIO_BCLK GPIO_NUM_21
#define AUDIO_WS GPIO_NUM_13
#define AUDIO_DIN GPIO_NUM_47
#define AUDIO_DOUT GPIO_NUM_14

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_xl9555;
static uint16_t s_xl9555_out;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_codec_dev_handle_t s_codec;
static SemaphoreHandle_t s_lcd_mutex;
static SemaphoreHandle_t s_lcd_done;
static uint16_t *s_lcd_frame;
static bool s_lcd_failed;

enum {
    LCD_WIDTH = 320,
    LCD_HEIGHT = 240,
    LCD_FRAME_BYTES = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
};

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static const uint8_t *font_glyph(char character)
{
    static const uint8_t glyphs[26][7] = {
        {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // A
        {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, // B
        {0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f}, // C
        {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, // D
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, // E
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, // F
        {0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f}, // G
        {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // H
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}, // I
        {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, // L
        {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // O
        {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, // P
        {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, // Q
        {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, // R
        {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, // S
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // U
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, // V
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, // W
        {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, // X
        {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, // Y
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, // Z
    };
    static const uint8_t zero[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
    static const uint8_t one[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
    static const uint8_t two[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
    if (character >= 'A' && character <= 'Z') {
        return glyphs[(unsigned)(character - 'A')];
    }
    if (character == '0') {
        return zero;
    }
    if (character == '1') {
        return one;
    }
    if (character == '2') return two;
    return NULL;
}

static void lcd_fill(uint16_t color)
{
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
        s_lcd_frame[i] = color;
    }
}

static void lcd_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > LCD_WIDTH) {
        width = LCD_WIDTH - x;
    }
    if (y + height > LCD_HEIGHT) {
        height = LCD_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            s_lcd_frame[row * LCD_WIDTH + column] = color;
        }
    }
}

static void lcd_text(const char *text, int x, int y, int scale, uint16_t color)
{
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        const char character = *cursor >= 'a' && *cursor <= 'z' ? *cursor - ('a' - 'A') : *cursor;
        const uint8_t *glyph = font_glyph(character);
        if (glyph != NULL) {
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if ((glyph[row] & (1u << (4 - column))) != 0) {
                        lcd_rect(x + column * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static esp_err_t xl9555_write_register(uint8_t reg, uint16_t value);

static esp_err_t xl9555_set_speaker(bool enabled)
{
    const uint16_t next = board_audio_outputs(s_xl9555_out, enabled);
    if (next == s_xl9555_out) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(xl9555_write_register(XL9555_REG_OUTPUT_0, next), TAG,
                        "XL9555 speaker switch failed");
    s_xl9555_out = next;
    return ESP_OK;
}

static esp_err_t xl9555_write_register(uint8_t reg, uint16_t value)
{
    uint8_t data[] = {reg, (uint8_t)(value & 0xff), (uint8_t)(value >> 8)};
    return i2c_master_transmit(s_xl9555, data, sizeof(data), 100);
}

static esp_err_t xl9555_read_register(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_xl9555, &reg, 1, data, sizeof(data), 100),
                        TAG, "XL9555 register read failed");
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

static esp_err_t xl9555_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "I2C bus init failed");

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &device_config, &s_xl9555),
                        TAG, "XL9555 device init failed");

    // Preload safe levels before making pins outputs; never pulse the buzzer.
    const uint16_t outputs = board_audio_outputs(BOARD_XIO_LCD_BACKLIGHT, false);
    ESP_RETURN_ON_ERROR(xl9555_write_register(XL9555_REG_OUTPUT_0, outputs),
                        TAG, "XL9555 output setup failed");
    s_xl9555_out = outputs;
    const uint16_t config = (uint16_t)~BOARD_XIO_OUTPUT_MASK;
    ESP_RETURN_ON_ERROR(xl9555_write_register(XL9555_REG_CONFIG_0, config), TAG,
                        "XL9555 direction setup failed");

    uint16_t input = 0;
    ESP_RETURN_ON_ERROR(xl9555_read_register(XL9555_REG_INPUT_0, &input), TAG,
                        "XL9555 probe failed");
    ESP_LOGI(TAG, "XL9555 ready at 0x%02x, input=0x%04x", XL9555_ADDR, input);
    return ESP_OK;
}

static bool lcd_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *event, void *context)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t lcd_init(void)
{
    gpio_config_t rd_config = {
        .pin_bit_mask = 1ULL << LCD_RD,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rd_config), TAG, "LCD RD setup failed");
    gpio_set_level(LCD_RD, 1);

    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_DC,
        .wr_gpio_num = LCD_WR,
        .data_gpio_nums = {LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_D4, LCD_D5, LCD_D6, LCD_D7},
        .bus_width = 8,
        .max_transfer_bytes = 320 * 240 * sizeof(uint16_t),
    };
    esp_lcd_i80_bus_handle_t i80_bus;
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_config, &i80_bus), TAG, "I80 bus init failed");

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .swap_color_bytes = 0,
        },
        .on_color_trans_done = lcd_transfer_done,
    };
    esp_lcd_panel_io_handle_t panel_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io), TAG,
                        "I80 panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(panel_io, &panel_config, &s_lcd_panel), TAG,
                        "ST7789 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "ST7789 reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "ST7789 panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG,
                        "ST7789 inversion setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, true), TAG,
                        "ST7789 orientation setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, true, false), TAG,
                        "ST7789 mirror setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG,
                        "ST7789 display-on failed");
    s_lcd_mutex = xSemaphoreCreateMutex();
    s_lcd_done = xSemaphoreCreateBinary();
    s_lcd_frame = heap_caps_malloc(LCD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_lcd_mutex == NULL || s_lcd_done == NULL || s_lcd_frame == NULL) {
        if (s_lcd_mutex != NULL) {
            vSemaphoreDelete(s_lcd_mutex);
            s_lcd_mutex = NULL;
        }
        if (s_lcd_done != NULL) {
            vSemaphoreDelete(s_lcd_done);
            s_lcd_done = NULL;
        }
        free(s_lcd_frame);
        s_lcd_frame = NULL;
        ESP_LOGW(TAG, "status display framebuffer unavailable");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "ST7789 I80 display ready");
    return ESP_OK;
}

esp_err_t board_display_status(const char *status, const char *theme)
{
    if (s_lcd_panel == NULL || s_lcd_mutex == NULL || s_lcd_frame == NULL || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t accent = rgb565(80, 180, 255);
    if (strcmp(status, "idle") == 0) {
        accent = rgb565(60, 210, 130);
    } else if (strcmp(status, "listening") == 0) {
        accent = rgb565(70, 150, 255);
    } else if (strcmp(status, "thinking") == 0) {
        accent = rgb565(180, 100, 255);
    } else if (strcmp(status, "speaking") == 0) {
        accent = rgb565(40, 220, 220);
    } else if (strcmp(status, "interrupted") == 0) {
        accent = rgb565(255, 190, 60);
    } else if (strcmp(status, "error") == 0) {
        accent = rgb565(255, 80, 80);
    } else if (strcmp(status, "wifi_connecting") == 0) {
        accent = rgb565(255, 170, 60);
    }

    if (xSemaphoreTake(s_lcd_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_lcd_failed) {
        xSemaphoreGive(s_lcd_mutex);
        return ESP_FAIL;
    }
    const uint16_t background = rgb565(10, 18, 32);
    lcd_fill(background);
    lcd_rect(0, 0, LCD_WIDTH, 10, accent);
    lcd_text("ICEBREAKER", 24, 30, 4, rgb565(220, 235, 245));
    const char *label = strcmp(status, "wifi_connecting") == 0 ? "CONNECTING" : status;
    lcd_text(label, 24, 90, strlen(label) > 8 ? 4 : 5, accent);
    if (theme != NULL) lcd_text(theme, 24, 139, 2, rgb565(220, 235, 245));
    lcd_rect(24, 164, 272, 2, accent);
    lcd_text("K0 TALK  K1 THEME", 24, 184, 2, rgb565(150, 170, 190));
    if (strcmp(status, "idle") == 0) {
        lcd_text("K2 JOIN - VOICE TO CLOUD", 24, 207, 1, rgb565(245, 190, 90));
    } else if (strcmp(status, "hosting") == 0) {
        lcd_text("K2 END ACTIVITY", 24, 207, 2, rgb565(245, 190, 90));
    } else {
        lcd_text("K2 JOIN / END", 24, 207, 2, rgb565(150, 170, 190));
    }
    esp_err_t result = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT,
                                                         s_lcd_frame);
    // draw_bitmap is asynchronous: do not redraw a buffer still owned by LCD DMA.
    if (result == ESP_OK && xSemaphoreTake(s_lcd_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        s_lcd_failed = true;
        result = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "LCD DMA timed out; further display updates disabled");
    }
    xSemaphoreGive(s_lcd_mutex);
    return result;
}

static esp_err_t audio_init(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx), TAG,
                        "I2S channel init failed");

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_WS,
            .dout = AUDIO_DOUT,
            .din = AUDIO_DIN,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &config), TAG,
                        "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &config), TAG,
                        "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S RX enable failed");

    audio_codec_i2c_cfg_t codec_i2c = {
        .port = I2C_NUM_0,
        // esp_codec_dev's I2C wrapper shifts this value right once; ES8311
        // uses the board's 7-bit address 0x18, so pass the 8-bit form 0x30.
        .addr = 0x30,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c);
    ESP_LOGI(TAG, "ES8311 I2C address configured as 0x%02x", codec_i2c.addr);
    audio_codec_i2s_cfg_t codec_i2s = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&codec_i2s);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (ctrl_if == NULL || data_if == NULL || gpio_if == NULL) {
        ESP_LOGW(TAG, "ES8311 interfaces unavailable");
        return ESP_OK;
    }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        .no_dac_ref = true,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    if (codec_if == NULL) {
        ESP_LOGW(TAG, "ES8311 interface unavailable");
        return ESP_OK;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    if (s_codec == NULL) {
        ESP_LOGW(TAG, "ES8311 device creation failed");
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 24000,
    };
    if (esp_codec_dev_open(s_codec, &sample) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 open failed");
        s_codec = NULL;
        return ESP_OK;
    }
    const int diagnostic_registers[] = {0x09, 0x0A, 0x14, 0x15, 0x16, 0x17, 0x1B, 0x1C};
    for (size_t i = 0; i < sizeof(diagnostic_registers) / sizeof(diagnostic_registers[0]); ++i) {
        int value = 0;
        if (esp_codec_dev_read_reg(s_codec, diagnostic_registers[i], &value) == ESP_CODEC_DEV_OK) {
            ESP_LOGI(TAG, "ES8311 reg 0x%02x = 0x%02x", diagnostic_registers[i], value);
        }
    }
    const int gain_result = esp_codec_dev_set_in_gain(s_codec, 7.0f);
    esp_codec_dev_set_out_vol(s_codec, 60);
    esp_codec_dev_set_out_mute(s_codec, true);
    ESP_LOGI(TAG, "ES8311 ready in duplex mode, muted, volume 60%%, input gain result=%d", gain_result);
    ESP_LOGI(TAG, "I2S channels ready: BCLK=%d WS=%d DIN=%d DOUT=%d",
             AUDIO_BCLK, AUDIO_WS, AUDIO_DIN, AUDIO_DOUT);
    return ESP_OK;
}

esp_err_t board_mic_probe(void)
{
    if (s_codec == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    enum { SAMPLE_RATE = 24000, DURATION_MS = 500 };
    const size_t frames = SAMPLE_RATE * DURATION_MS / 1000;
    const size_t bytes = frames * sizeof(int16_t);
    int16_t *buffer = calloc(1, bytes);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_set_out_mute(s_codec, true);
    xl9555_set_speaker(false);
    const int result = esp_codec_dev_read(s_codec, buffer, bytes);
    if (result != ESP_CODEC_DEV_OK) {
        free(buffer);
        ESP_LOGW(TAG, "microphone read failed: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mic raw: %d %d %d %d %d %d %d %d",
             buffer[0], buffer[1], buffer[2], buffer[3],
             buffer[4], buffer[5], buffer[6], buffer[7]);

    double sum = 0;
    int16_t peak = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const int16_t value = buffer[frame];
        sum += (double)value * value;
        const int16_t magnitude = value < 0 ? (int16_t)-value : value;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    free(buffer);

    ESP_LOGI(TAG, "mic: rms=%d peak=%d", (int)(sqrt(sum / frames) + 0.5), peak);
    return ESP_OK;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(xl9555_init(), TAG, "XL9555 initialization failed");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD initialization failed");
    ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio initialization failed");
    return ESP_OK;
}

esp_err_t board_read_audio(int16_t *samples, size_t sample_count)
{
    if (s_codec == NULL || samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_codec_dev_set_out_mute(s_codec, true);
    xl9555_set_speaker(false);
    return esp_codec_dev_read(s_codec, samples,
                              sample_count * sizeof(int16_t)) == ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t board_play_pcm_mono(const int16_t *samples, size_t sample_count)
{
    if (s_codec == NULL || samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int result = esp_codec_dev_write(s_codec, (void *)samples,
                                           sample_count * sizeof(int16_t));
    if (result != ESP_CODEC_DEV_OK) {
        board_stop_audio();
        ESP_LOGW(TAG, "PCM playback failed: result=%d samples=%u", result,
                 (unsigned)sample_count);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t board_start_audio(void)
{
    if (s_codec == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (esp_codec_dev_set_out_mute(s_codec, false) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    const esp_err_t result = xl9555_set_speaker(true);
    if (result != ESP_OK) {
        esp_codec_dev_set_out_mute(s_codec, true);
    }
    return result;
}

void board_stop_audio(void)
{
    // Cut the amplifier first, independently of codec register update latency.
    xl9555_set_speaker(false);
    if (s_codec != NULL) {
        esp_codec_dev_set_out_mute(s_codec, true);
    }
    ESP_LOGI(TAG, "audio output muted locally");
}

esp_err_t board_get_audio_outputs(uint16_t *outputs)
{
    return xl9555_read_register(XL9555_REG_OUTPUT_0, outputs);
}

esp_err_t board_read_activity_buttons(uint8_t *buttons)
{
    uint16_t inputs;
    ESP_RETURN_ON_ERROR(xl9555_read_register(XL9555_REG_INPUT_0, &inputs), TAG, "key read failed");
    *buttons = ((inputs & (1u << 4)) == 0 ? (1u << 1) : 0) |
               ((inputs & (1u << 3)) == 0 ? (1u << 2) : 0);
    return ESP_OK;
}
