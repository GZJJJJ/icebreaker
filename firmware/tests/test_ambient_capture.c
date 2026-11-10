#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "ambient_capture.h"

int main(void)
{
    ambient_capture_t *capture = calloc(1, sizeof(*capture));
    assert(capture != NULL);
    int16_t pcm[AMBIENT_CHUNK_SAMPLES] = {123};
    for (unsigned i = 0; i < 1000; ++i) {
        assert(ambient_capture_push(capture, pcm, false) == 0);
    }
    assert(capture->chunks == AMBIENT_PRE_CHUNKS && !capture->active);
    assert(ambient_capture_push(capture, pcm, true) == AMBIENT_STARTED);
    for (unsigned i = 1; i < 4; ++i) assert(ambient_capture_push(capture, pcm, true) == 0);
    for (unsigned i = 0; i < 7; ++i) assert(ambient_capture_push(capture, pcm, false) == 0);
    assert(ambient_capture_push(capture, pcm, false) == AMBIENT_READY);
    assert(capture->chunks == AMBIENT_PRE_CHUNKS + 4); // Trailing silence excluded.
    ambient_capture_reset(capture);
    assert(capture->chunks == 0 && capture->pcm[0] == 0);
    ambient_capture_push(capture, pcm, true);
    for (unsigned i = 0; i < 8; ++i) assert(ambient_capture_push(capture, pcm, false) == 0);
    assert(!capture->active && capture->chunks == 0); // Brief noise discarded.
    for (unsigned i = 0; i < AMBIENT_MAX_CHUNKS; ++i) {
        const unsigned result = ambient_capture_push(capture, pcm, true);
        assert(result == (i == 0 ? AMBIENT_STARTED :
                          i == AMBIENT_MAX_CHUNKS - 1 ? AMBIENT_READY : 0));
    }
    assert(capture->chunks == AMBIENT_MAX_CHUNKS);
    ambient_capture_reset(capture);
    free(capture);
    puts("PASS: silence, preroll, segmentation, noise rejection, max length and clearing");
    return 0;
}
