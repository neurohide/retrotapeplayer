#include "tap.h"

#include <assert.h>
#include <string.h>

typedef struct {
    unsigned count;
    unsigned pilot;
    unsigned zero;
    unsigned one;
    unsigned sync;
    unsigned pauses;
    unsigned cancel_at;
    unsigned blocks;
    long position;
    long total;
    unsigned data_count;
    uint32_t data_pulses[80];
} capture_t;

static bool capture(uint32_t duration, bool silence, void *context)
{
    capture_t *state = context;
    ++state->count;
    if (state->cancel_at && state->count >= state->cancel_at) return false;
    if (silence) {
        assert(duration == 3500000);
        ++state->pauses;
    } else {
        switch (duration) {
        case 2168: ++state->pilot; break;
        case 667: case 735: ++state->sync; break;
        case 855: case 1710:
            assert(state->data_count < 80);
            state->data_pulses[state->data_count++] = duration;
            if (duration == 855) ++state->zero;
            else ++state->one;
            break;
        default: assert(false);
        }
    }
    return true;
}

static void progress(long position, long total, unsigned block, void *context)
{
    capture_t *state = context;
    state->blocks = block;
    state->position = position;
    state->total = total;
}

static tap_result_t play(const unsigned char *data, size_t length, capture_t *state)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(data, 1, length, file) == length);
    tap_result_t result = tap_play(file, capture, progress, state);
    fclose(file);
    return result;
}

int main(void)
{
    const unsigned char valid[] = {2, 0, 0, 0, 3, 0, 255, 128, 127};
    capture_t state = {0};
    assert(play(valid, sizeof(valid), &state) == TAP_OK);
    assert(state.pilot == 8063 + 3223);
    assert(state.zero == 48 && state.one == 32);
    assert(state.sync == 4 && state.pauses == 2 && state.blocks == 2);
    assert(state.position == sizeof(valid) && state.total == sizeof(valid));
    assert(state.data_count == 80);
    for (unsigned index = 0; index < 80; ++index) {
        bool one = (index >= 32 && index < 50) || index >= 66;
        assert(state.data_pulses[index] == (one ? 1710 : 855));
    }
    const unsigned char invalid[][5] = {
        {0, 0, 0, 0, 0}, {1, 0, 0, 0, 0}, {2, 0, 0, 1, 0},
        {255, 255, 0, 0, 0}
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        memset(&state, 0, sizeof(state));
        assert(play(invalid[index], sizeof(invalid[index]), &state) == TAP_INVALID);
        assert(state.count == 0);
    }
    assert(play(valid, 0, &state) == TAP_INVALID);
    assert(play(valid, 1, &state) == TAP_INVALID);
    assert(play(valid, sizeof(valid) - 1, &state) == TAP_INVALID);
    state = (capture_t){.cancel_at = 10};
    assert(play(valid, sizeof(valid), &state) == TAP_CANCELLED);
    assert(state.count == 10);
    puts("TAP tests passed");
    return 0;
}