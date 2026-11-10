#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_TEXT_MAX 80
#define DISPLAY_TEXT_MISSING 0x25a1

// Strict UTF-8 decoding. Every invalid input advances at least one byte.
static inline uint32_t display_next_codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    if (*p == 0) return 0;
    const unsigned char lead = *p++;
    *text = (const char *)p;
    if (lead < 0x80) return lead;
    const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 1 :
                           lead >= 0xe0 && lead <= 0xef ? 2 :
                           lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
    if (!count) return DISPLAY_TEXT_MISSING;
    uint32_t code = lead & ((1u << (6 - count)) - 1);
    for (unsigned i = 0; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80) return DISPLAY_TEXT_MISSING;
        code = (code << 6) | (p[i] & 0x3f);
    }
    *text += count;
    if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
        (count == 3 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) return DISPLAY_TEXT_MISSING;
    return code;
}

// Same ranges/order as tools/build_display_font.py; each glyph is 32 bytes.
static inline size_t display_glyph_index(uint32_t code)
{
    static const uint16_t ranges[][2] = {
        {0x20, 0x7e}, {0xb7, 0xb7}, {0x2000, 0x206f}, {0x25a1, 0x25a1},
        {0x3000, 0x303f}, {0x4e00, 0x9fff}, {0xff00, 0xffef},
    };
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        if (code >= ranges[i][0] && code <= ranges[i][1]) return offset + code - ranges[i][0];
        offset += ranges[i][1] - ranges[i][0] + 1;
    }
    return 95 + 1 + 112; // U+25A1, a visible fallback rather than a blank.
}

static inline size_t display_text_decode(uint16_t out[DISPLAY_TEXT_MAX + 1], const char *text)
{
    size_t count = 0;
    if (text != NULL) {
        while (*text && count < DISPLAY_TEXT_MAX) {
            uint32_t code = display_next_codepoint(&text);
            if (code <= 0x20 || code == 0x7f || code == 0x3000) {
                if (!count || out[count - 1] == ' ') continue;
                code = ' ';
            } else if (code > 0xffff) {
                code = DISPLAY_TEXT_MISSING;
            }
            out[count++] = (uint16_t)code;
        }
        if (*text && count) out[count - 1] = 0x2026;
    }
    while (count && out[count - 1] == ' ') --count;
    out[count] = 0;
    return count;
}

typedef void (*display_glyph_callback_t)(uint16_t code, int x, int y, void *context);

static inline void display_text_layout(const uint16_t *text, int x, int y, int width,
                                       unsigned lines, display_glyph_callback_t draw, void *context)
{
    const int left = x;
    unsigned row = 0;
    for (size_t i = 0; text[i]; ++i) {
        const int advance = text[i] < 0x80 ? 8 : 16;
        if (x + advance > left + width) { x = left; y += 20; ++row; }
        if (row >= lines) break;
        draw(text[i], x, y, context);
        x += advance;
    }
}
