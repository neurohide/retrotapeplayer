#include "cas.h"

#include <assert.h>
#include <string.h>

#define MAX_PULSES 60000

typedef struct {
    unsigned count;
    unsigned cancel_at;
    uint32_t durations[MAX_PULSES];
    bool silences[MAX_PULSES];
    unsigned blocks;
    long position;
    long total;
} capture_t;

static capture_t state;

static bool capture(uint32_t duration, bool silence, void *context)
{
    capture_t *capture = context;
    if (capture->cancel_at && capture->count + 1 >= capture->cancel_at) {
        ++capture->count;
        return false;
    }
    assert(capture->count < MAX_PULSES);
    capture->durations[capture->count] = duration;
    capture->silences[capture->count++] = silence;
    return true;
}

static void progress(long position, long total, unsigned block, void *context)
{
    capture_t *capture = context;
    capture->blocks = block;
    capture->position = position;
    capture->total = total;
}

static tap_result_t play(const unsigned char *data, size_t length)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(data, 1, length, file) == length);
    tap_result_t result = cas_play(file, capture, progress, &state);
    fclose(file);
    return result;
}

// Checks `count` half cycles of `length` x 1/4800 s from `start`; returns the next index.
static unsigned expect_halves(unsigned start, unsigned count, unsigned length)
{
    for (unsigned index = start; index < start + count; ++index) {
        assert(!state.silences[index]);
        uint32_t duration = state.durations[index];
        assert(length == 1 ? duration == 729 || duration == 730 : duration == 1458 || duration == 1459);
    }
    return start + count;
}

static unsigned expect_byte(unsigned index, uint8_t value)
{
    index = expect_halves(index, 2, 2);
    for (unsigned bit = 0; bit < 8; ++bit) {
        index = (value >> bit) & 1 ? expect_halves(index, 4, 1) : expect_halves(index, 2, 2);
    }
    return expect_halves(index, 8, 1);
}

int main(void)
{
    const unsigned char marker[] = {0x1f, 0xa6, 0xde, 0xba, 0xcc, 0x13, 0x7d, 0x74};
    unsigned char image[64];
    size_t length = 0;
    // Stray bytes before the first marker are skipped.
    image[length++] = 0x55;
    image[length++] = 0xaa;
    memcpy(image + length, marker, 8);
    length += 8;
    memset(image + length, 0xd3, 10);
    length += 10;
    memcpy(image + length, "HELLO ", 6);
    length += 6;
    memcpy(image + length, marker, 8);
    length += 8;
    image[length++] = 0x01;
    image[length++] = 0xfe;

    assert(play(image, length) == TAP_OK);
    assert(state.blocks == 2 && state.position == (long)length && state.total == (long)length);
    // First block: 2 s of silence, 16000 cycles of 2400 Hz, then 16 bytes.
    assert(state.silences[0] && state.durations[0] == 7000000);
    unsigned index = expect_halves(1, 32000, 1);
    uint64_t header_tstates = 0;
    for (unsigned half = 1; half < index; ++half) header_tstates += state.durations[half];
    assert(header_tstates == 32000ull * 3500000 / 4800 || header_tstates == 32000ull * 3500000 / 4800 + 1);
    for (unsigned byte = 0; byte < 10; ++byte) index = expect_byte(index, 0xd3);
    for (unsigned byte = 0; byte < 6; ++byte) index = expect_byte(index, "HELLO "[byte]);
    // Second block has no file type: 1 s of silence and 4000 cycles.
    assert(state.silences[index] && state.durations[index] == 3500000);
    index = expect_halves(index + 1, 8000, 1);
    index = expect_byte(index, 0x01);
    index = expect_byte(index, 0xfe);
    assert(state.silences[index] && state.durations[index] == 3500000);
    assert(index + 1 == state.count);

    // No marker, or an empty file, is not a CAS image.
    memset(&state, 0, sizeof(state));
    assert(play(image, 9) == TAP_INVALID && state.count == 0);
    assert(play(image, 0) == TAP_INVALID && state.count == 0);

    memset(&state, 0, sizeof(state));
    state.cancel_at = 10;
    assert(play(image, length) == TAP_CANCELLED && state.count == 10);
    assert(strcmp(cas_result_text(TAP_INVALID), "INVALID CAS FILE") == 0);
    puts("CAS tests passed");
    return 0;
}
