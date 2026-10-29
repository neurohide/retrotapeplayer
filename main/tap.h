#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef bool (*tap_pulse_fn)(uint32_t tstates, bool silence, void *context);
typedef void (*tap_progress_fn)(long position, long total, unsigned block, void *context);

typedef enum {
    TAP_OK,
    TAP_CANCELLED,
    TAP_INVALID,
    TAP_IO_ERROR,
    TAP_NO_MEMORY,
    TAP_UNSUPPORTED
} tap_result_t;

tap_result_t tap_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, void *context);
const char *tap_result_text(tap_result_t result);