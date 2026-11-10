#pragma once

#include <string.h>
#include "display_text.h"

typedef struct {
    uint16_t text[DISPLAY_TEXT_MAX + 1];
    bool pending;
    uint32_t request_id;
} hint_state_t;

static inline void hint_state_clear(hint_state_t *hint)
{
    hint->pending = false;
    memset(hint->text, 0, sizeof(hint->text));
}

static inline bool hint_state_ready(const hint_state_t *hint) { return hint->text[0] != 0; }

static inline uint32_t hint_state_request(hint_state_t *hint)
{
    hint_state_clear(hint);
    if (++hint->request_id == 0) ++hint->request_id;
    hint->pending = true;
    return hint->request_id;
}

static inline bool hint_state_accept(hint_state_t *hint, uint32_t request_id, const char *text)
{
    if (!hint->pending || request_id != hint->request_id) return false;
    hint->pending = false;
    return display_text_decode(hint->text, text) != 0;
}
