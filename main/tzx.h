#pragma once

#include "tap.h"

// Called for a "stop the tape" block (pause 0). Returns false to cancel playback.
typedef bool (*tzx_stop_fn)(void *context);

// Plays a ZX Spectrum .tzx image through the same pulse and progress callbacks as
// tap_play (durations in 3.5 MHz T-states). `stop` may be NULL to ignore stops.
tap_result_t tzx_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, tzx_stop_fn stop,
                      void *context);
const char *tzx_result_text(tap_result_t result);
