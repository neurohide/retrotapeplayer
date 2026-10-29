# CLAUDE.md

ZX Spectrum `.tap` / `.tzx` and MSX `.cas` cassette player firmware for the ESP32-2432S028R ("Cheap
Yellow Display": classic ESP32, ILI9341 LCD, XPT2046 touch). ESP-IDF 5.4, plain
C, no external components. README.md is the user-facing manual (usage, pinout,
calibration file format, TAP timings); read it before changing behaviour it
documents, and keep it in sync.

## Commands

```sh
idf.py build                          # needs ESP-IDF 5.4 env (IDF_PATH); see .vscode/settings.json
idf.py -p <port> flash monitor        # flashes firmware AND the sdcard/ FAT image

# Host-side decoder tests (no ESP-IDF needed)
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c tests/tap_test.c -o /tmp/tap_test && /tmp/tap_test
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c main/tzx.c tests/tzx_test.c -o /tmp/tzx_test && /tmp/tzx_test
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c main/cas.c tests/cas_test.c -o /tmp/cas_test && /tmp/cas_test
```

Simulation: build, then **Wokwi: Start Simulator** in VS Code (`wokwi.toml`
points at `build/flasher_args.json` and `build/tape_player.elf`). Headless runs
use `wokwi-cli --scenario <file.yaml> .` with `WOKWI_CLI_TOKEN` set; scenario
`touch`/`touch-press` steps and `take-screenshot` use part id `lcd1`, with
coordinates in FT6206 portrait space: screen (sx, sy) → (239 − sy, 319 − sx).
Hold scripted presses ≥ 150 ms; a full redraw takes ~90 ms of simulated time.

Do not switch the diagram back to Wokwi's `board-esp32-2432s028r` part: its
XPT2046 never drives MISO/PENIRQ (reads float to all-0 or all-1) and its SD slot
is unwired. `diagram.json` instead rebuilds the board from a DevKitC V4, an
`ili9341-cap-touch` (FT6206 on CN1 pins 27/22) and a microSD card on the real
SD pins. The card is filled from `sdcard/` each start. Keep `writeback` off in
`wokwi.toml`: it restores the start-of-run card on stop/restart (the VS Code
extension restarts on every rebuild) and deleted files added to `sdcard/`
meanwhile. With the FT6206 the firmware uses a fixed mapping and skips the
calibration screen unless `calibration.json` exists. DAC audio, the
XPT2046 path, and loading on a real Spectrum can only be verified on hardware;
the task watchdog warns about `tap_audio` during simulated playback.

`sdkconfig` is generated and gitignored; put config changes in
`sdkconfig.defaults` (delete `sdkconfig` to regenerate from it). The defaults
select `-O2` and a 240 MHz CPU, mainly to keep full-screen redraws fast.

## Layout

- `main/tap.c`, `main/tap.h` — portable TAP decoder. Reads length-prefixed
  blocks, verifies XOR checksums, and emits pulses as T-state durations at
  3.5 MHz through a `tap_pulse_fn` callback (returning `false` cancels).
  **Must stay free of ESP-IDF dependencies** so `tests/tap_test.c` builds on the
  host. Add a test case there when changing decoder behaviour.
- `main/tzx.c`, `main/tzx.h` — portable TZX decoder, same callbacks plus a
  `tzx_stop_fn` for "stop the tape" blocks (main.c's `tape_stop` sets
  `pause_playback` and waits in `audio_pause`). It indexes block offsets first
  (for jumps/loops/calls), then streams with `fgetc`. TZX pulses are "edge, then
  hold": the held duration stays in `pending` until the next edge so 0x15/0x19/
  0x2B can extend it; `flush()` emits it before silence and must reset it.
  Tested by `tests/tzx_test.c`, which also checks 0x10 blocks against `tap_play`.
- `main/cas.c`, `main/cas.h` — portable MSX CAS decoder (1200 baud FSK, timings
  from openMSX), same callbacks and `tap_result_t` as the TAP decoder and the
  same no-ESP-IDF rule; tested by `tests/cas_test.c`. It streams through a
  512-byte buffer instead of loading blocks. `main.c` picks the decoder by file
  extension (`has_extension`). All three decoders share `tap_result_t`
  (including `TAP_UNSUPPORTED`).
- `main/main.c` — everything else, in one file: ILI9341 driver, framebuffer
  drawing primitives and 5×7 pixel font, touch (bit-banged XPT2046, or an
  FT6206 over I2C when one answers at startup),
  three-point affine touch calibration (cJSON load/save), SD / flash FAT
  mounting and file scanning, DAC audio output, and the UI (library browser,
  cassette view, calibration screen).
- `main/CMakeLists.txt` — also builds `sdcard/` into the `storage` partition via
  `fatfs_create_rawflash_image(... FLASH_IN_PROJECT)`.
- `partitions.csv` — 1.5 MB app (`factory`) + 2 MB read-only FAT (`storage`);
  requires 4 MB flash. Files in `sdcard/` must fit in 2 MB.
- `sdcard/` — sample TAP bundled into flash (the BUILT-IN TAPES library) and
  copied onto the simulator's microSD card.
- `diagram.json`, `wokwi.toml` — Wokwi simulation setup (see above).
- `main/cassette-tdk-d46.bin`, `main/cassette-sony-hf60.bin` — generated,
  committed: the playback screen's cassette photos plus 24 pre-rotated frames
  of each hub, embedded with `EMBED_FILES`. Made from `assets/*.jpg` by
  `tools/cassette_image.py` (README has the commands; the Sony source must be
  scaled to 1920 px first). Crop/hub constants live per photo in its `PHOTOS`
  table, measured on those exact files. The TDK photo is CC BY 3.0, credited in
  README; the Sony photo's source/licence is not recorded yet.
- `main/splash.bin` — generated, committed: the start-up splash icon (200×200,
  white made transparent) from `assets/splash.jpeg`, same script and format with
  no hubs. `render_splash()` draws it with `V<PROJECT_VER> - APP_AUTHOR - <build
  year from esp_app_get_description()>`; `app_main` keeps it up for at least
  `SPLASH_MS` while mounting the SD card, then calls `select_cassette(0)` to put
  the cassette colours back before anything else draws.

## Architecture notes and pitfalls

**Two tasks.** `ui_task` (core 1, prio 3) polls touch every 20 ms and redraws
at most every 125 ms. `playback_task` (core 0, prio 5) is created per playback,
runs `tap_play` → `audio_pulse` → `dac_continuous_write`, and deletes itself.
Shared `player` state is guarded by the `state_lock` spinlock — read it via
`player_snapshot()`, write it inside `portENTER/EXIT_CRITICAL`. Stop is the
atomic `cancel_playback`, checked inside `audio_pulse`; pause is the atomic
`pause_playback`, where `audio_pause()` keeps writing silence so the output
rests at the midpoint. A paused player still counts as playing. The DAC channel
is created once (`audio_output()`) and never deleted: re-creating it made the
DMA EOF interrupt push a stale descriptor address into the new channel's pool,
which crashed `dac_continuous_write` on the second playback. Each playback ends
by writing one silence buffer, since the output keeps its last samples once the
DMA runs dry. The playback task reads
`files[selected]` and `library_root` directly, so nothing may rescan or change
the selection while playing (the browser is only reachable once playback has
stopped).

**Buses.** SPI2 drives the LCD, SPI3 the SD card. The XPT2046 is bit-banged on
GPIOs 25/32/39/33 (IRQ 36). `touch_i2c_init()` probes I2C0 on the CN1 pins
(SDA 27, SCL 22) for an FT6206 at 0x38; if found, `touch_inputs()` reads it
instead and reports TD_STATUS as PENIRQ with zero pressure (the FT6206 keeps
the last coordinates after release, so pressure must not count as a press).
Both backends feed the same affine calibration, so `calibration.json` belongs
to whichever controller wrote it.

**Calibration loop** checks for a press before redrawing; a redraw between
detecting a press and `touch_sample()` loses short taps.

**Framebuffer is 8-bit indexed.** `frame` is 320×240 palette indices; drawing
functions take RGB565 colors and `rect()` looks them up in `palette[]` (the 7 UI
colors). A color not in `palette[]` silently draws as index 0 (`INK`) — add new
colors to both the `#define`s and `palette[]`. Indices 7–254 are the cassette
photo's colors and 255 is transparent; `palette_init()` builds the byte-swapped
`lcd_palette[256]` that `display_flush()` uses, and `select_cassette()` swaps in
a photo's colors (indices 7+) and pointers via `load_photo()`; only one photo
(a cassette or the splash) can be on screen at a time. It is called only from the UI task
(on each library tap, `tapes_played` alternates the photos), so it cannot race
a flush. Adding a UI color shifts the photos' indices, so regenerate every
`cassette-*.bin` with a matching `FIRST_INDEX` (`palette_init()` checks all of
them at boot and aborts on a mismatch). `rect()` clips; `image()` does not.
The `.bin` files are not aligned in flash: read them with `memcpy`/bytes, never
by casting to `uint16_t *`.

**Font** covers only A–Z, 0–9, space and `. : - _ / %`; lowercase is
uppercased and anything else renders as `?`. Glyph cells are 6 px × scale wide.

**Touch hit areas are hard-coded** in `handle_tap()` (and the FILES check in
`ui_task`) and must match the coordinates used in `render_browser()` /
`render_cassette()`. Change both together.

**Audio** is 250 kHz 8-bit DAC on GPIO26; levels 48/208 for pulse edges, 128 for
silence. `audio.remainder` carries fractional samples between pulses — keep
that accumulation when touching timing. Only one TAP block is in RAM at a time.

**Storage.** SD mounts at `/sdcard` with `format_if_mount_failed = false` —
never change that to true. There is no card-detect pin: `update_card()` polls
from `ui_task` every second, only while not playing (the playback task reads the
card), mounting when absent and sending `sdmmc_get_status` when mounted.
`card_state` is MISSING (mount timed out or got an invalid response — the real
board times out, Wokwi without a card answers garbage), UNSUPPORTED (anything
else, or a volume that is not FAT16/FAT32) or READY; the first two show
`render_card_screen()` unless `builtin_library` is set. To simulate an empty
slot, remove the `sd1` part from a copy of `diagram.json` and pass it with
`wokwi-cli --diagram-file`. The built-in library mounts read-only at `/flash`. Only
regular, non-dot `.tap`, `.tzx` and `.cas` files in the root are listed (max 96, case-insensitive sort).
`mount_sd()` also reads the volume label into `card_label` with `f_getlabel`,
which needs `CONFIG_FATFS_USE_LABEL=y` (in `sdkconfig.defaults`);
`render_label()` shows it under the header title.
`calibration.json` is read/written only on the SD card: `apply_card_calibration()`
runs whenever a card becomes READY (startup or `update_card()`), loading the file
or running `calibrate_touch()` (XPT2046 only) and saving it. No card means no
calibration screen; touch keeps the default (or last applied) mapping.

## Style

- `main.c` is indented with **tabs**; `tap.c`, `tzx.c`, `cas.c`, their headers and the tests use **4
  spaces**. Match the file you are editing.
- Linux-ish brace style for functions, K&R for blocks; short single-statement
  `if`s on one line are common.
- Descriptive full-word names (`index`, `screen_x`, `palette_index`), `static`
  for everything file-local, comments sparse and only for non-obvious reasons.
- On-screen strings are uppercase and short; status strings must fit
  `player.status` (40 bytes).
