#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hint_state.h"

static unsigned glyph_count;
static int max_y;
static void draw(uint16_t code, int x, int y, void *context)
{
    (void)context;
    assert(code != 0);
    assert(x >= 24 && x + (code < 0x80 ? 8 : 16) <= 296);
    assert(y >= 77 && y + 16 <= 173);
    if (y > max_y) max_y = y;
    ++glyph_count;
}

int main(void)
{
    hint_state_t hint = {0};
    uint32_t first = hint_state_request(&hint);
    assert(!hint_state_accept(&hint, first + 1, "stale"));
    assert(hint.pending && !hint_state_ready(&hint));
    assert(hint_state_accept(&hint, first, "最近有没有一件让你觉得有趣的小事？"));
    assert(hint_state_ready(&hint) && !hint.pending);
    assert(hint.text[0] == 0x6700 && hint.text[18] == 0);
    assert(!hint_state_accept(&hint, first, "duplicate"));
    hint_state_clear(&hint); // Speech, dismiss, pause, end and disconnect use this path.
    for (unsigned i = 0; i <= DISPLAY_TEXT_MAX; ++i) assert(hint.text[i] == 0);
    assert(!hint_state_accept(&hint, first, "late after dismiss"));
    uint32_t second = hint_state_request(&hint);
    assert(second != first);
    assert(!hint_state_accept(&hint, first, "old request while new one pending"));
    assert(hint_state_accept(&hint, second, "  \r\n旅行\t电影  "));
    assert(hint.text[0] == 0x65c5 && hint.text[2] == ' ' && hint.text[5] == 0);
    assert(display_text_decode(hint.text, " \r\t\n") == 0);
    const char truncated[] = {(char)0xe4, (char)0xb8, 0};
    assert(display_text_decode(hint.text, truncated) == 2);
    assert(hint.text[0] == DISPLAY_TEXT_MISSING);
    assert(display_text_decode(hint.text, "A\xf0\x9f\x98\x80中") == 3);
    assert(hint.text[1] == DISPLAY_TEXT_MISSING && hint.text[2] == 0x4e2d);
    const char *overlong = "\xc0\x80";
    assert(display_next_codepoint(&overlong) == DISPLAY_TEXT_MISSING);
    char long_text[301];
    for (unsigned i = 0; i < 100; ++i) memcpy(long_text + 3 * i, "中", 3);
    long_text[300] = 0;
    assert(display_text_decode(hint.text, long_text) == 80);
    assert(hint.text[79] == 0x2026 && hint.text[80] == 0);
    display_text_layout(hint.text, 24, 77, 272, 5, draw, NULL);
    assert(glyph_count == 80 && max_y == 157);
    assert(display_glyph_index(' ') == 0);
    assert(display_glyph_index(0x4e00) == 273);
    assert(display_glyph_index(0x1f600) == display_glyph_index(DISPLAY_TEXT_MISSING));
    hint.request_id = UINT32_MAX;
    assert(hint_state_request(&hint) == 1);
    assert(!hint_state_accept(&hint, 1, " \t\r\n"));
    puts("PASS: hint ownership, correlation, clear, UTF-8, truncation, wrapping and font indices");
}
