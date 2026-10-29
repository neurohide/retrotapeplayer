#include "tzx.h"

#include <stdlib.h>
#include <string.h>

#define CLOCK 3500000
#define MS_TSTATES 3500
#define HEADER_SIZE 0x14

typedef struct {
    FILE *file;
    long total;
    tap_pulse_fn pulse;
    tap_progress_fn progress;
    void *context;
    unsigned block;
    // TZX pulses are "edge, then hold". The held duration is kept back until the
    // next edge, so blocks that continue or force the level can extend it.
    uint32_t pending;
    bool has_pending;
    bool high;
} player_t;

typedef struct {
    uint32_t pilot;
    uint32_t sync1;
    uint32_t sync2;
    uint32_t zero;
    uint32_t one;
    unsigned pilot_count;
    unsigned used_bits;
    unsigned pause;
    uint32_t length;
} data_block_t;

static uint32_t le(const uint8_t *data, unsigned bytes)
{
    uint32_t value = 0;
    while (bytes--) {
        value = value << 8 | data[bytes];
    }
    return value;
}

static tap_result_t read_failure(FILE *file)
{
    return ferror(file) ? TAP_IO_ERROR : TAP_INVALID;
}

static bool edge(player_t *player, uint32_t duration)
{
    if (player->has_pending && !player->pulse(player->pending, false, player->context)) {
        return false;
    }
    player->high = !player->high;
    player->pending = duration;
    player->has_pending = true;
    return true;
}

static void extend(player_t *player, uint32_t duration)
{
    player->pending += duration;
    player->has_pending = true;
}

// Ends the held pulse. The audio output starts low after silence, so the next edge
// is treated as going low.
static bool flush(player_t *player)
{
    bool running = !player->has_pending || player->pulse(player->pending, false, player->context);
    player->pending = 0;
    player->has_pending = false;
    player->high = true;
    return running;
}

static bool pause_ms(player_t *player, unsigned milliseconds)
{
    if (!milliseconds) {
        return true;
    }
    return flush(player) && player->pulse((uint32_t)milliseconds * MS_TSTATES, true, player->context);
}

static bool tone(player_t *player, uint32_t duration, unsigned count)
{
    for (unsigned index = 0; index < count; ++index) {
        if (!edge(player, duration)) {
            return false;
        }
    }
    return true;
}

static void report(player_t *player)
{
    if (player->progress) {
        player->progress(ftell(player->file), player->total, player->block, player->context);
    }
}

// Two edges per bit, most significant bit first; the last byte may use fewer bits.
static tap_result_t play_bits(player_t *player, uint32_t zero, uint32_t one, unsigned used_bits,
                              uint32_t length)
{
    if (used_bits < 1 || used_bits > 8) {
        used_bits = 8;
    }
    for (uint32_t index = 0; index < length; ++index) {
        int value = fgetc(player->file);
        if (value == EOF) {
            return read_failure(player->file);
        }
        unsigned bits = index + 1 == length ? used_bits : 8;
        for (unsigned bit = 0; bit < bits; ++bit) {
            uint32_t duration = (value & (0x80 >> bit)) ? one : zero;
            if (!edge(player, duration) || !edge(player, duration)) {
                return TAP_CANCELLED;
            }
        }
        if ((index + 1) % 256 == 0) {
            report(player);
        }
    }
    return TAP_OK;
}

static tap_result_t play_data_block(player_t *player, const data_block_t *block)
{
    ++player->block;
    report(player);
    if (!tone(player, block->pilot, block->pilot_count)) {
        return TAP_CANCELLED;
    }
    if (block->sync1 && (!edge(player, block->sync1) || !edge(player, block->sync2))) {
        return TAP_CANCELLED;
    }
    tap_result_t result = play_bits(player, block->zero, block->one, block->used_bits, block->length);
    if (result == TAP_OK && !pause_ms(player, block->pause)) {
        return TAP_CANCELLED;
    }
    return result;
}

// Block 0x15: one bit per sample, giving the signal level.
static tap_result_t play_direct(player_t *player, uint32_t sample, unsigned used_bits,
                                uint32_t length, unsigned pause)
{
    if (used_bits < 1 || used_bits > 8) {
        used_bits = 8;
    }
    ++player->block;
    report(player);
    for (uint32_t index = 0; index < length; ++index) {
        int value = fgetc(player->file);
        if (value == EOF) {
            return read_failure(player->file);
        }
        unsigned bits = index + 1 == length ? used_bits : 8;
        for (unsigned bit = 0; bit < bits; ++bit) {
            bool level = value & (0x80 >> bit);
            if (level == player->high) {
                extend(player, sample);
            } else if (!edge(player, sample)) {
                return TAP_CANCELLED;
            }
        }
    }
    return pause_ms(player, pause) ? TAP_OK : TAP_CANCELLED;
}

// Block 0x18: a CSW recording. Only RLE compression is supported, not Z-RLE.
static tap_result_t play_csw(player_t *player, uint32_t length)
{
    uint8_t header[10];
    if (length < sizeof(header) || fread(header, 1, sizeof(header), player->file) != sizeof(header)) {
        return read_failure(player->file);
    }
    uint32_t rate = le(header + 2, 3);
    if (!rate) {
        return TAP_INVALID;
    }
    if (header[5] != 1) {
        return TAP_UNSUPPORTED;
    }
    ++player->block;
    report(player);
    uint64_t remainder = 0;
    for (uint32_t left = length - sizeof(header); left;) {
        int value = fgetc(player->file);
        if (value == EOF) {
            return read_failure(player->file);
        }
        --left;
        uint32_t samples = value;
        if (!samples) {
            uint8_t wide[4];
            if (left < 4 || fread(wide, 1, 4, player->file) != 4) {
                return read_failure(player->file);
            }
            left -= 4;
            samples = le(wide, 4);
        }
        remainder += (uint64_t)samples * CLOCK;
        if (!edge(player, remainder / rate)) {
            return TAP_CANCELLED;
        }
        remainder %= rate;
    }
    return pause_ms(player, le(header, 2)) ? TAP_OK : TAP_CANCELLED;
}

// One symbol of a generalized data block. Flags in bits 0-1 decide whether its
// first pulse starts with an edge: 0 always, 1 never, 2 only if high, 3 only if low.
static bool play_symbol(player_t *player, const uint8_t *symbol, unsigned pulses)
{
    unsigned flags = symbol[0] & 3;
    for (unsigned index = 0; index < pulses; ++index) {
        uint32_t duration = le(symbol + 1 + 2 * index, 2);
        if (!duration) {
            break;
        }
        bool starts_edge = index || flags == 0 || (flags == 2 && player->high)
                           || (flags == 3 && !player->high);
        if (!starts_edge) {
            extend(player, duration);
        } else if (!edge(player, duration)) {
            return false;
        }
    }
    return true;
}

// Block 0x19: symbol tables for the pilot/sync and data parts, a run-length pilot
// stream, and a packed data stream of ceil(log2(symbols)) bits per symbol.
static tap_result_t play_generalized(player_t *player, uint32_t length)
{
    long end = ftell(player->file) + (long)length;
    uint8_t header[14];
    if (length < sizeof(header) || fread(header, 1, sizeof(header), player->file) != sizeof(header)) {
        return read_failure(player->file);
    }
    uint32_t pilot_symbols = le(header + 2, 4);
    unsigned pilot_pulses = header[6];
    unsigned pilot_alphabet = header[7] ? header[7] : 256;
    uint32_t data_symbols = le(header + 8, 4);
    unsigned data_pulses = header[12];
    unsigned data_alphabet = header[13] ? header[13] : 256;
    size_t pilot_size = (size_t)(1 + 2 * pilot_pulses);
    size_t data_size = (size_t)(1 + 2 * data_pulses);
    uint8_t *table = malloc((pilot_size > data_size ? pilot_size : data_size) * 256);
    if (!table) {
        return TAP_NO_MEMORY;
    }
    ++player->block;
    report(player);
    tap_result_t result = TAP_OK;
    if (pilot_symbols) {
        if (fread(table, pilot_size, pilot_alphabet, player->file) != pilot_alphabet) {
            result = read_failure(player->file);
        }
        for (uint32_t index = 0; index < pilot_symbols && result == TAP_OK; ++index) {
            uint8_t entry[3];
            if (fread(entry, 1, 3, player->file) != 3) {
                result = read_failure(player->file);
            } else if (entry[0] >= pilot_alphabet) {
                result = TAP_INVALID;
            }
            for (unsigned count = le(entry + 1, 2); result == TAP_OK && count; --count) {
                if (!play_symbol(player, table + entry[0] * pilot_size, pilot_pulses)) {
                    result = TAP_CANCELLED;
                }
            }
        }
    }
    if (data_symbols && result == TAP_OK) {
        if (fread(table, data_size, data_alphabet, player->file) != data_alphabet) {
            result = read_failure(player->file);
        }
        unsigned bits = 0;
        while ((1u << bits) < data_alphabet) {
            ++bits;
        }
        int byte = 0;
        unsigned available = 0;
        for (uint32_t index = 0; index < data_symbols && result == TAP_OK; ++index) {
            unsigned symbol = 0;
            for (unsigned bit = 0; bit < bits && result == TAP_OK; ++bit) {
                if (!available) {
                    byte = fgetc(player->file);
                    available = 8;
                    if (byte == EOF) {
                        result = read_failure(player->file);
                    }
                }
                symbol = symbol << 1 | ((byte >> --available) & 1);
            }
            if (result == TAP_OK && symbol >= data_alphabet) {
                result = TAP_INVALID;
            } else if (result == TAP_OK && !play_symbol(player, table + symbol * data_size, data_pulses)) {
                result = TAP_CANCELLED;
            }
            if (result == TAP_OK && (index + 1) % 2048 == 0) {
                report(player);
            }
        }
    }
    free(table);
    if (result == TAP_OK && fseek(player->file, end, SEEK_SET) != 0) {
        result = TAP_IO_ERROR;
    }
    if (result == TAP_OK && !pause_ms(player, le(header, 2))) {
        result = TAP_CANCELLED;
    }
    return result;
}

// Reads `length` header bytes of the block body and rewinds to its start.
static bool peek(FILE *file, uint8_t *data, size_t length)
{
    long start = ftell(file);
    return fread(data, 1, length, file) == length && fseek(file, start, SEEK_SET) == 0;
}

// Size of a block body (after its ID byte), or -1 if the file ends early.
static long block_size(FILE *file, int id)
{
    uint8_t header[HEADER_SIZE];
    switch (id) {
    case 0x10: return peek(file, header, 4) ? 4 + (long)le(header + 2, 2) : -1;
    case 0x11: return peek(file, header, 0x12) ? 0x12 + (long)le(header + 0x0f, 3) : -1;
    case 0x12: return 4;
    case 0x13: return peek(file, header, 1) ? 1 + 2 * (long)header[0] : -1;
    case 0x14: return peek(file, header, 0x0a) ? 0x0a + (long)le(header + 7, 3) : -1;
    case 0x15: return peek(file, header, 8) ? 8 + (long)le(header + 5, 3) : -1;
    case 0x20: case 0x23: case 0x24: return 2;
    case 0x21: case 0x30: return peek(file, header, 1) ? 1 + (long)header[0] : -1;
    case 0x22: case 0x25: case 0x27: return 0;
    case 0x26: return peek(file, header, 2) ? 2 + 2 * (long)le(header, 2) : -1;
    case 0x28: case 0x32: return peek(file, header, 2) ? 2 + (long)le(header, 2) : -1;
    case 0x31: return peek(file, header, 2) ? 2 + (long)header[1] : -1;
    case 0x33: return peek(file, header, 1) ? 1 + 3 * (long)header[0] : -1;
    case 0x34: return 8;
    case 0x35: return peek(file, header, 0x14) ? 0x14 + (long)le(header + 0x10, 4) : -1;
    case 0x40: return peek(file, header, 4) ? 4 + (long)le(header + 1, 3) : -1;
    case 0x5a: return 9;
    // 0x18, 0x19, 0x2a, 0x2b and any later block start with a 32-bit length.
    default: return peek(file, header, 4) ? 4 + (long)le(header, 4) : -1;
    }
}

// Records where each block starts, so jumps, loops and calls can go by number.
static tap_result_t index_blocks(FILE *file, long total, long **offsets, unsigned *count)
{
    unsigned capacity = 0;
    *offsets = NULL;
    *count = 0;
    while (ftell(file) < total) {
        long start = ftell(file);
        int id = fgetc(file);
        long size = id == EOF ? -1 : block_size(file, id);
        if (size < 0 || ftell(file) + size > total) {
            return read_failure(file);
        }
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            long *grown = realloc(*offsets, capacity * sizeof(**offsets));
            if (!grown) {
                return TAP_NO_MEMORY;
            }
            *offsets = grown;
        }
        (*offsets)[(*count)++] = start;
        if (fseek(file, ftell(file) + size, SEEK_SET) != 0) {
            return TAP_IO_ERROR;
        }
    }
    return *count ? TAP_OK : TAP_INVALID;
}

static tap_result_t play_blocks(player_t *player, const long *offsets, unsigned count,
                                tzx_stop_fn stop)
{
    FILE *file = player->file;
    unsigned loop_start = 0;
    unsigned loop_count = 0;
    unsigned call_block = 0;
    unsigned call_count = 0;
    unsigned call_number = 0;
    tap_result_t result = TAP_OK;
    for (unsigned index = 0; index < count && result == TAP_OK;) {
        uint8_t header[HEADER_SIZE];
        long next = (long)index + 1;
        int id;
        if (fseek(file, offsets[index], SEEK_SET) != 0 || (id = fgetc(file)) == EOF) {
            return read_failure(file);
        }
        size_t needed = 0;
        switch (id) {
        case 0x10: case 0x12: case 0x18: case 0x19: needed = 4; break;
        case 0x11: needed = 0x12; break;
        case 0x13: needed = 1; break;
        case 0x14: needed = 0x0a; break;
        case 0x15: needed = 8; break;
        case 0x20: case 0x23: case 0x24: case 0x26: needed = 2; break;
        case 0x2b: needed = 5; break;
        }
        if (needed && fread(header, 1, needed, file) != needed) {
            return read_failure(file);
        }
        switch (id) {
        case 0x10: {
            // Standard speed data: ROM timings, with the pilot length set by the flag byte.
            int flag = fgetc(file);
            if (flag == EOF || ungetc(flag, file) == EOF) {
                return read_failure(file);
            }
            data_block_t block = {2168, 667, 735, 855, 1710, flag < 128 ? 8063 : 3223, 8,
                                  le(header, 2), le(header + 2, 2)};
            result = play_data_block(player, &block);
            break;
        }
        case 0x11: {
            data_block_t block = {le(header, 2), le(header + 2, 2), le(header + 4, 2), le(header + 6, 2),
                                  le(header + 8, 2), le(header + 10, 2), header[12],
                                  le(header + 13, 2), le(header + 15, 3)};
            result = play_data_block(player, &block);
            break;
        }
        case 0x12:
            if (!tone(player, le(header, 2), le(header + 2, 2))) result = TAP_CANCELLED;
            break;
        case 0x13:
            for (unsigned pulse = 0; pulse < header[0] && result == TAP_OK; ++pulse) {
                uint8_t duration[2];
                if (fread(duration, 1, 2, file) != 2) result = read_failure(file);
                else if (!edge(player, le(duration, 2))) result = TAP_CANCELLED;
            }
            break;
        case 0x14: {
            data_block_t block = {0, 0, 0, le(header, 2), le(header + 2, 2), 0, header[4],
                                  le(header + 5, 2), le(header + 7, 3)};
            result = play_data_block(player, &block);
            break;
        }
        case 0x15:
            result = play_direct(player, le(header, 2), header[4], le(header + 5, 3), le(header + 2, 2));
            break;
        case 0x18:
            result = play_csw(player, le(header, 4));
            break;
        case 0x19:
            result = play_generalized(player, le(header, 4));
            break;
        case 0x20:
            if (le(header, 2)) {
                if (!pause_ms(player, le(header, 2))) result = TAP_CANCELLED;
            } else if (!flush(player) || (stop && !stop(player->context))) {
                result = TAP_CANCELLED;
            }
            break;
        case 0x23:
            if (!(int16_t)le(header, 2)) return TAP_INVALID;
            next = (long)index + (int16_t)le(header, 2);
            break;
        case 0x24:
            loop_count = le(header, 2);
            loop_start = index + 1;
            break;
        case 0x25:
            if (loop_count > 1) {
                --loop_count;
                next = loop_start;
            }
            break;
        case 0x26:
            // Call sequence: play the listed blocks, each ending at a return block.
            call_count = le(header, 2);
            call_block = index;
            call_number = 0;
            if (call_count) {
                if (fread(header + 2, 1, 2, file) != 2) return read_failure(file);
                next = (long)index + (int16_t)le(header + 2, 2);
            }
            break;
        case 0x27:
            if (call_count && ++call_number < call_count) {
                uint8_t offset[2];
                if (fseek(file, offsets[call_block] + 3 + 2 * call_number, SEEK_SET) != 0
                    || fread(offset, 1, 2, file) != 2) {
                    return read_failure(file);
                }
                next = (long)call_block + (int16_t)le(offset, 2);
            } else if (call_count) {
                next = (long)call_block + 1;
                call_count = 0;
            }
            break;
        case 0x2b:
            if ((header[4] != 0) != player->high && !edge(player, 0)) result = TAP_CANCELLED;
            break;
        default:
            // Groups, text, archive info, "stop if 48K" and other metadata.
            break;
        }
        if (next < 0 || next > (long)count) {
            return TAP_INVALID;
        }
        index = (unsigned)next;
    }
    return result;
}

tap_result_t tzx_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, tzx_stop_fn stop,
                      void *context)
{
    if (!file || !pulse || fseek(file, 0, SEEK_END) != 0) {
        return TAP_IO_ERROR;
    }
    long total = ftell(file);
    if (total < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return TAP_IO_ERROR;
    }
    uint8_t signature[10];
    if (fread(signature, 1, sizeof(signature), file) != sizeof(signature)) {
        return read_failure(file);
    }
    if (memcmp(signature, "ZXTape!\x1a", 8) != 0 || signature[8] != 1) {
        return TAP_INVALID;
    }
    long *offsets;
    unsigned count;
    tap_result_t result = index_blocks(file, total, &offsets, &count);
    if (result == TAP_OK) {
        player_t player = {
            .file = file, .total = total, .pulse = pulse, .progress = progress, .context = context,
            .high = true
        };
        result = play_blocks(&player, offsets, count, stop);
        if (result == TAP_OK && !flush(&player)) {
            result = TAP_CANCELLED;
        }
        if (result == TAP_OK && progress) {
            progress(total, total, player.block, context);
        }
    }
    free(offsets);
    return result;
}

const char *tzx_result_text(tap_result_t result)
{
    return result == TAP_INVALID ? "INVALID TZX FILE" : tap_result_text(result);
}
