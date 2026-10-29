#include "cas.h"

#include <string.h>

// MSX tapes at 1200 baud: a 0 bit is one 1200 Hz cycle, a 1 bit two 2400 Hz cycles.
// Lengths follow openMSX: 2 s of silence and 16000 cycles of 2400 Hz before the
// first block of a file, 1 s and 4000 cycles before the blocks that follow.
#define CLOCK 3500000
#define HALF_CYCLES_PER_SECOND 4800
#define LONG_HEADER 16000
#define SHORT_HEADER 4000
#define BUFFER_SIZE 512
#define MARKER_SIZE 8
#define TYPE_SIZE 10

static const uint8_t marker[MARKER_SIZE] = {0x1f, 0xa6, 0xde, 0xba, 0xcc, 0x13, 0x7d, 0x74};

typedef struct {
    tap_pulse_fn pulse;
    void *context;
    uint32_t remainder;
} output_t;

// Emits half cycles of `length` x 1/4800 s, carrying the fractional T-states.
static bool half_cycles(output_t *output, unsigned count, unsigned length)
{
    for (unsigned index = 0; index < count; ++index) {
        output->remainder += length * CLOCK;
        uint32_t duration = output->remainder / HALF_CYCLES_PER_SECOND;
        output->remainder %= HALF_CYCLES_PER_SECOND;
        if (!output->pulse(duration, false, output->context)) {
            return false;
        }
    }
    return true;
}

static bool write_bit(output_t *output, bool one)
{
    return one ? half_cycles(output, 4, 1) : half_cycles(output, 2, 2);
}

// Start bit, eight data bits least significant first, two stop bits.
static bool write_byte(output_t *output, uint8_t value)
{
    bool running = write_bit(output, false);
    for (unsigned bit = 0; bit < 8 && running; ++bit) {
        running = write_bit(output, (value >> bit) & 1);
    }
    return running && write_bit(output, true) && write_bit(output, true);
}

static bool write_header(output_t *output, bool long_header)
{
    if (!output->pulse(long_header ? 2 * CLOCK : CLOCK, true, output->context)) {
        return false;
    }
    return half_cycles(output, 2 * (long_header ? LONG_HEADER : SHORT_HEADER), 1);
}

// Ten equal bytes after a marker name the file type: ASCII, binary or BASIC.
static bool file_type(const uint8_t *data)
{
    if (data[0] != 0xea && data[0] != 0xd0 && data[0] != 0xd3) {
        return false;
    }
    for (unsigned index = 1; index < TYPE_SIZE; ++index) {
        if (data[index] != data[0]) {
            return false;
        }
    }
    return true;
}

tap_result_t cas_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, void *context)
{
    if (!file || !pulse || fseek(file, 0, SEEK_END) != 0) {
        return TAP_IO_ERROR;
    }
    long total = ftell(file);
    if (total < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return TAP_IO_ERROR;
    }
    output_t output = {.pulse = pulse, .context = context};
    uint8_t buffer[BUFFER_SIZE];
    size_t used = 0;
    size_t offset = 0;
    long position = 0;
    unsigned block = 0;
    for (;;) {
        // Keep enough bytes buffered to recognise a marker and the file type after it.
        if (used - offset < MARKER_SIZE + TYPE_SIZE && !feof(file)) {
            memmove(buffer, buffer + offset, used - offset);
            used -= offset;
            offset = 0;
            used += fread(buffer + used, 1, sizeof(buffer) - used, file);
            if (ferror(file)) {
                return TAP_IO_ERROR;
            }
        }
        size_t available = used - offset;
        if (!available) {
            break;
        }
        bool running = true;
        if (available >= MARKER_SIZE && !memcmp(buffer + offset, marker, MARKER_SIZE)) {
            offset += MARKER_SIZE;
            position += MARKER_SIZE;
            ++block;
            if (progress) {
                progress(position, total, block, context);
            }
            bool typed = available >= MARKER_SIZE + TYPE_SIZE && file_type(buffer + offset);
            running = write_header(&output, block == 1 || typed);
        } else {
            // Bytes before the first marker are not part of any block.
            if (block) {
                running = write_byte(&output, buffer[offset]);
            }
            ++offset;
            ++position;
            if (running && progress && block && position % 256 == 0) {
                progress(position, total, block, context);
            }
        }
        if (!running) {
            return TAP_CANCELLED;
        }
    }
    if (!block) {
        return TAP_INVALID;
    }
    if (!pulse(CLOCK, true, context)) {
        return TAP_CANCELLED;
    }
    if (progress) {
        progress(total, total, block, context);
    }
    return TAP_OK;
}

const char *cas_result_text(tap_result_t result)
{
    return result == TAP_INVALID ? "INVALID CAS FILE" : tap_result_text(result);
}
