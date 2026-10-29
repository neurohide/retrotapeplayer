#include "tzx.h"

#include <assert.h>
#include <string.h>

#define MAX_PULSES 20000

typedef struct {
    unsigned count;
    unsigned cancel_at;
    uint32_t durations[MAX_PULSES];
    bool silences[MAX_PULSES];
    unsigned stops;
    bool stop_result;
    unsigned blocks;
    long position;
    long total;
} capture_t;

static capture_t tap_state;
static capture_t tzx_state;

static bool capture(uint32_t duration, bool silence, void *context)
{
    capture_t *state = context;
    if (state->cancel_at && state->count + 1 >= state->cancel_at) {
        ++state->count;
        return false;
    }
    assert(state->count < MAX_PULSES);
    state->durations[state->count] = duration;
    state->silences[state->count++] = silence;
    return true;
}

static bool stop(void *context)
{
    capture_t *state = context;
    ++state->stops;
    return state->stop_result;
}

static void progress(long position, long total, unsigned block, void *context)
{
    capture_t *state = context;
    state->blocks = block;
    state->position = position;
    state->total = total;
}

static FILE *file_with(const unsigned char *data, size_t length)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite(data, 1, length, file) == length);
    return file;
}

static tap_result_t play(const unsigned char *data, size_t length, capture_t *state)
{
    FILE *file = file_with(data, length);
    tap_result_t result = tzx_play(file, capture, progress, stop, state);
    fclose(file);
    return result;
}

typedef struct {
    unsigned char data[512];
    size_t length;
} image_t;

static void bytes(image_t *image, const void *data, size_t length)
{
    assert(image->length + length <= sizeof(image->data));
    memcpy(image->data + image->length, data, length);
    image->length += length;
}

static void byte(image_t *image, unsigned value)
{
    unsigned char data = value;
    bytes(image, &data, 1);
}

static void word(image_t *image, unsigned value)
{
    byte(image, value & 0xff);
    byte(image, value >> 8);
}

static void triple(image_t *image, unsigned value)
{
    word(image, value & 0xffff);
    byte(image, value >> 16);
}

static void dword(image_t *image, unsigned value)
{
    word(image, value & 0xffff);
    word(image, value >> 16);
}

static image_t tzx(void)
{
    image_t image = {0};
    bytes(&image, "ZXTape!\x1a\x01\x14", 10);
    return image;
}

static void expect(const capture_t *state, const uint32_t *durations, size_t count)
{
    assert(state->count == count);
    for (size_t index = 0; index < count; ++index) {
        // Silences are marked with the top bit in the expectation.
        assert(state->silences[index] == !!(durations[index] & 0x80000000u));
        assert(state->durations[index] == (durations[index] & 0x7fffffffu));
    }
}

#define SILENCE(duration) ((duration) | 0x80000000u)

int main(void)
{
    // Standard speed blocks sound exactly like the same blocks in a TAP file.
    const unsigned char tap[] = {2, 0, 0, 0, 3, 0, 255, 128, 127};
    FILE *file = file_with(tap, sizeof(tap));
    assert(tap_play(file, capture, NULL, &tap_state) == TAP_OK);
    fclose(file);
    image_t standard = tzx();
    byte(&standard, 0x10), word(&standard, 1000), word(&standard, 2), bytes(&standard, "\0\0", 2);
    byte(&standard, 0x10), word(&standard, 1000), word(&standard, 3), bytes(&standard, "\xff\x80\x7f", 3);
    assert(play(standard.data, standard.length, &tzx_state) == TAP_OK);
    assert(tzx_state.count == tap_state.count);
    assert(!memcmp(tzx_state.durations, tap_state.durations, tap_state.count * sizeof(uint32_t)));
    assert(!memcmp(tzx_state.silences, tap_state.silences, tap_state.count * sizeof(bool)));
    assert(tzx_state.blocks == 2 && tzx_state.position == (long)standard.length);

    // Every other sound block, with loops, metadata and a stop.
    image_t mixed = tzx();
    byte(&mixed, 0x12), word(&mixed, 100), word(&mixed, 3);
    byte(&mixed, 0x13), byte(&mixed, 2), word(&mixed, 200), word(&mixed, 300);
    byte(&mixed, 0x24), word(&mixed, 2);
    byte(&mixed, 0x12), word(&mixed, 50), word(&mixed, 1);
    byte(&mixed, 0x25);
    byte(&mixed, 0x30), byte(&mixed, 2), bytes(&mixed, "HI", 2);
    byte(&mixed, 0x21), byte(&mixed, 1), byte(&mixed, 'G');
    byte(&mixed, 0x22);
    // Turbo: two pilot pulses, syncs, two used bits of 0x80 (1 then 0), no pause.
    byte(&mixed, 0x11), word(&mixed, 1000), word(&mixed, 400), word(&mixed, 500), word(&mixed, 600),
        word(&mixed, 1200), word(&mixed, 2), byte(&mixed, 2), word(&mixed, 0), triple(&mixed, 1),
        byte(&mixed, 0x80);
    // Pure data 0x01 and a 5 ms pause.
    byte(&mixed, 0x14), word(&mixed, 700), word(&mixed, 1400), byte(&mixed, 8), word(&mixed, 5),
        triple(&mixed, 1), byte(&mixed, 0x01);
    byte(&mixed, 0x20), word(&mixed, 0);
    // Direct recording: four high then four low samples of 10 T-states.
    byte(&mixed, 0x15), word(&mixed, 10), word(&mixed, 0), byte(&mixed, 8), triple(&mixed, 1),
        byte(&mixed, 0xf0);
    // Generalized data: pilot symbol (80, 90) twice; data symbols 0 = edge 30,
    // 1 = no edge 40 then edge 45; data stream 0, 1, 0. Symbol 1 extends the 30
    // before it into one 70 T-state pulse.
    byte(&mixed, 0x19), dword(&mixed, 14 + 5 + 3 + 10 + 1);
    word(&mixed, 0), dword(&mixed, 1), byte(&mixed, 2), byte(&mixed, 1), dword(&mixed, 3),
        byte(&mixed, 2), byte(&mixed, 2);
    byte(&mixed, 0), word(&mixed, 80), word(&mixed, 90);
    byte(&mixed, 0), word(&mixed, 2);
    byte(&mixed, 0), word(&mixed, 30), word(&mixed, 0);
    byte(&mixed, 1), word(&mixed, 40), word(&mixed, 45);
    byte(&mixed, 0x40);
    // CSW RLE at 350 kHz (10 T-states per sample): 5, 20 (long form), 7 samples.
    byte(&mixed, 0x18), dword(&mixed, 10 + 7);
    word(&mixed, 0), triple(&mixed, 350000), byte(&mixed, 1), dword(&mixed, 3);
    byte(&mixed, 5), byte(&mixed, 0), dword(&mixed, 20), byte(&mixed, 7);
    // A metadata block in the 32-bit length form ("stop if 48K").
    byte(&mixed, 0x2a), dword(&mixed, 0);
    memset(&tzx_state, 0, sizeof(tzx_state));
    tzx_state.stop_result = true;
    assert(play(mixed.data, mixed.length, &tzx_state) == TAP_OK);
    uint32_t expected[] = {
        100, 100, 100, 200, 300, 50, 50,
        1000, 1000, 400, 500, 1200, 1200, 600, 600,
        700, 700, 700, 700, 700, 700, 700, 700, 700, 700, 700, 700, 700, 700, 1400, 1400,
        SILENCE(5 * 3500),
        40, 40,
        80, 90, 80, 90, 70, 45, 30,
        50, 200, 70
    };
    expect(&tzx_state, expected, sizeof(expected) / sizeof(expected[0]));
    assert(tzx_state.stops == 1 && tzx_state.blocks == 5);
    assert(tzx_state.position == (long)mixed.length);

    // Jumps and a call sequence: 0 jumps to 2, which calls 4 then 6; 3 jumps past the end.
    image_t calls = tzx();
    byte(&calls, 0x23), word(&calls, 2);
    byte(&calls, 0x12), word(&calls, 999), word(&calls, 1);
    byte(&calls, 0x26), word(&calls, 2), word(&calls, 2), word(&calls, 4);
    byte(&calls, 0x23), word(&calls, 5);
    byte(&calls, 0x12), word(&calls, 11), word(&calls, 1);
    byte(&calls, 0x27);
    byte(&calls, 0x12), word(&calls, 22), word(&calls, 1);
    byte(&calls, 0x27);
    memset(&tzx_state, 0, sizeof(tzx_state));
    assert(play(calls.data, calls.length, &tzx_state) == TAP_OK);
    const uint32_t called[] = {11, 22};
    expect(&tzx_state, called, 2);

    // A stop that cancels, and cancellation from the pulse callback.
    memset(&tzx_state, 0, sizeof(tzx_state));
    assert(play(mixed.data, mixed.length, &tzx_state) == TAP_CANCELLED && tzx_state.stops == 1);
    memset(&tzx_state, 0, sizeof(tzx_state));
    tzx_state.cancel_at = 5;
    assert(play(mixed.data, mixed.length, &tzx_state) == TAP_CANCELLED && tzx_state.count == 5);

    // Z-RLE CSW is reported as unsupported.
    image_t compressed = tzx();
    byte(&compressed, 0x18), dword(&compressed, 10), word(&compressed, 0), triple(&compressed, 44100),
        byte(&compressed, 2), dword(&compressed, 0);
    memset(&tzx_state, 0, sizeof(tzx_state));
    assert(play(compressed.data, compressed.length, &tzx_state) == TAP_UNSUPPORTED);

    // Bad signature, no blocks, truncated blocks, and a zero jump.
    memset(&tzx_state, 0, sizeof(tzx_state));
    image_t empty = tzx();
    assert(play(empty.data, empty.length, &tzx_state) == TAP_INVALID);
    assert(play(standard.data, 9, &tzx_state) == TAP_INVALID);
    unsigned char wrong[sizeof(standard.data)];
    memcpy(wrong, standard.data, standard.length);
    wrong[0] = 'X';
    assert(play(wrong, standard.length, &tzx_state) == TAP_INVALID);
    assert(play(standard.data, standard.length - 1, &tzx_state) == TAP_INVALID);
    assert(play(mixed.data, 14, &tzx_state) == TAP_INVALID);
    image_t loop = tzx();
    byte(&loop, 0x23), word(&loop, 0);
    assert(play(loop.data, loop.length, &tzx_state) == TAP_INVALID);
    assert(tzx_state.count == 0);
    assert(strcmp(tzx_result_text(TAP_INVALID), "INVALID TZX FILE") == 0);
    puts("TZX tests passed");
    return 0;
}
