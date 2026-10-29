#include <assert.h>
#include <stdio.h>

#include "board_audio_control.h"

int main(void)
{
    for (unsigned outputs = 0; outputs <= UINT16_MAX; ++outputs) {
        for (int enabled = 0; enabled <= 1; ++enabled) {
            const uint16_t next = board_audio_outputs((uint16_t)outputs, enabled);
            assert((next & (1u << 2)) != 0); // BEEP must always stay off (active-low).
            assert(((next & (1u << 5)) != 0) == enabled); // Speaker CTRL is active-high.
            assert(((next ^ outputs) & (uint16_t)~((1u << 2) | (1u << 5))) == 0);
        }
    }
    assert(board_audio_outputs(BOARD_XIO_LCD_BACKLIGHT, false) == 0x0084);
    assert(board_audio_outputs(BOARD_XIO_LCD_BACKLIGHT, true) == 0x00a4);
    assert((BOARD_XIO_OUTPUT_MASK & ((1u << 3) | (1u << 4))) == 0); // K1/K2 stay inputs.
    puts("PASS: 131072 output transitions; buzzer off, speaker correct, other pins unchanged");
    return 0;
}
