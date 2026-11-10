#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AMBIENT_CHUNK_SAMPLES 1600
#define AMBIENT_MAX_CHUNKS 80
#define AMBIENT_PRE_CHUNKS 3
#define AMBIENT_QUIET_CHUNKS 8
#define AMBIENT_MIN_VOICE_CHUNKS 4
#define AMBIENT_STARTED 1
#define AMBIENT_READY 2

typedef struct {
    int16_t pcm[AMBIENT_MAX_CHUNKS * AMBIENT_CHUNK_SAMPLES];
    size_t chunks;
    size_t last_voice_chunks;
    unsigned voice_chunks;
    unsigned quiet_chunks;
    bool active;
} ambient_capture_t;

static inline void ambient_capture_reset(ambient_capture_t *capture)
{
    memset(capture, 0, sizeof(*capture));
}

// One 100 ms PCM16/16 kHz chunk. Caller consumes READY before pushing again.
static inline unsigned ambient_capture_push(ambient_capture_t *capture,
                                            const int16_t *pcm, bool voice)
{
    unsigned result = 0;
    if (!capture->active && voice) {
        capture->active = true;
        result |= AMBIENT_STARTED;
    }
    if (!capture->active && capture->chunks == AMBIENT_PRE_CHUNKS) {
        memmove(capture->pcm, capture->pcm + AMBIENT_CHUNK_SAMPLES,
                (AMBIENT_PRE_CHUNKS - 1) * AMBIENT_CHUNK_SAMPLES * sizeof(int16_t));
        capture->chunks--;
    }
    memcpy(capture->pcm + capture->chunks * AMBIENT_CHUNK_SAMPLES, pcm,
           AMBIENT_CHUNK_SAMPLES * sizeof(int16_t));
    capture->chunks++;
    if (!capture->active) return result;
    if (voice) {
        capture->voice_chunks++;
        capture->quiet_chunks = 0;
        capture->last_voice_chunks = capture->chunks;
    } else {
        capture->quiet_chunks++;
    }
    if (capture->chunks == AMBIENT_MAX_CHUNKS ||
        capture->quiet_chunks == AMBIENT_QUIET_CHUNKS) {
        if (capture->voice_chunks >= AMBIENT_MIN_VOICE_CHUNKS) {
            capture->chunks = capture->last_voice_chunks;
            result |= AMBIENT_READY;
        } else {
            ambient_capture_reset(capture);
        }
    }
    return result;
}
