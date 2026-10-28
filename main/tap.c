#include "tap.h"

#include <stdlib.h>

tap_result_t tap_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, void *context)
{
    if (!file || !pulse || fseek(file, 0, SEEK_END) != 0) {
        return TAP_IO_ERROR;
    }
    long total = ftell(file);
    if (total < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return TAP_IO_ERROR;
    }
    unsigned block = 0;
    while (ftell(file) < total) {
        int low = fgetc(file);
        int high = fgetc(file);
        if (low == EOF || high == EOF) {
            return ferror(file) ? TAP_IO_ERROR : TAP_INVALID;
        }
        size_t length = (unsigned)low | ((unsigned)high << 8);
        if (length < 2 || (long)length > total - ftell(file)) {
            return TAP_INVALID;
        }
        uint8_t *data = malloc(length);
        if (!data) {
            return TAP_NO_MEMORY;
        }
        if (fread(data, 1, length, file) != length) {
            free(data);
            return TAP_IO_ERROR;
        }
        uint8_t checksum = 0;
        for (size_t index = 0; index < length; ++index) {
            checksum ^= data[index];
        }
        if (checksum) {
            free(data);
            return TAP_INVALID;
        }
        ++block;
        if (progress) {
            progress(ftell(file) - (long)length, total, block, context);
        }
        bool running = true;
        unsigned pilot_count = data[0] < 128 ? 8063 : 3223;
        for (unsigned index = 0; index < pilot_count && running; ++index) {
            running = pulse(2168, false, context);
        }
        if (running) running = pulse(667, false, context);
        if (running) running = pulse(735, false, context);
        for (size_t index = 0; index < length && running; ++index) {
            for (unsigned bit = 0; bit < 8 && running; ++bit) {
                uint32_t duration = (data[index] & (0x80 >> bit)) ? 1710 : 855;
                running = pulse(duration, false, context);
                if (running) running = pulse(duration, false, context);
            }
            if (running && progress && (index + 1) % 256 == 0) {
                progress(ftell(file) - (long)length + (long)index + 1, total, block, context);
            }
        }
        free(data);
        if (!running || !pulse(3500000, true, context)) {
            return TAP_CANCELLED;
        }
        if (progress) {
            progress(ftell(file), total, block, context);
        }
    }
    return block ? TAP_OK : TAP_INVALID;
}

const char *tap_result_text(tap_result_t result)
{
    switch (result) {
    case TAP_OK: return "FINISHED";
    case TAP_CANCELLED: return "STOPPED";
    case TAP_INVALID: return "INVALID TAP / CHECKSUM";
    case TAP_IO_ERROR: return "SD READ ERROR";
    case TAP_NO_MEMORY: return "NOT ENOUGH MEMORY";
    default: return "PLAYBACK ERROR";
    }
}