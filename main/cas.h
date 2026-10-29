#pragma once

#include "tap.h"

// Plays an MSX .cas image as a 1200 baud MSX tape signal, through the same pulse
// and progress callbacks as tap_play (durations in 3.5 MHz T-states).
tap_result_t cas_play(FILE *file, tap_pulse_fn pulse, tap_progress_fn progress, void *context);
const char *cas_result_text(tap_result_t result);
