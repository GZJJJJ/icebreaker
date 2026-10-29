#pragma once

#include <stdbool.h>
#include <stdint.h>

// DNESP32S3B V1.1 schematic, DEVICE/POWER sheets (not DNESP32S3 V1.2).
#define BOARD_XIO_BEEP (1u << 2)
#define BOARD_XIO_SPEAKER (1u << 5)
#define BOARD_XIO_LCD_BACKLIGHT (1u << 7)
#define BOARD_XIO_OUTPUT_MASK (BOARD_XIO_BEEP | BOARD_XIO_SPEAKER | BOARD_XIO_LCD_BACKLIGHT)

static inline uint16_t board_audio_outputs(uint16_t outputs, bool enabled)
{
    // BEEP drives a PNP transistor: high is silent. NS4150B CTRL is active-high.
    outputs |= BOARD_XIO_BEEP;
    return enabled ? (outputs | BOARD_XIO_SPEAKER)
                   : (outputs & (uint16_t)~BOARD_XIO_SPEAKER);
}
