# Tape Player

ZX Spectrum TAP cassette player for the **ESP32-2432S028R** (classic ESP32,
ILI9341 / XPT2046 "Cheap Yellow Display"), using ESP-IDF 5.4.

## Use

1. Format a microSD card as **FAT32** and put ZX Spectrum `.tap` files in its
	root directory. Insert it before powering on the board.
2. Build with **ESP-IDF: Build your project**, then use **ESP-IDF: Flash your
	project**. Connect a speaker to the board's speaker connector.
3. The library lists only regular `.tap` files, case-insensitively, sorted by
	filename. Touch a row to hide the library and start playback immediately.
4. The cassette label displays the selected filename. Reels animate while
	playing; the progress bar and block number update during playback.
5. Touch the square to stop, the triangle to replay from the beginning, or
	**FILES** to stop and return to the library. Library arrows change pages;
	**RESCAN** retries mounting and refreshes the list. All controls are on the
	touchscreen.

Long filenames are enabled in FATFS. The library holds up to 96 files, five per
page; `96 MAX` indicates additional files were not listed. Subdirectories are
not scanned. Long names scroll on the cassette label during playback. The
pixel font renders ASCII names in uppercase; other characters appear as `?`.
Missing cards, empty directories, malformed TAP blocks, bad checksums, and
read/audio failures have explicit display states. A failed mount **never
formats the card**. Stop playback before removing a card; restart the board
after swapping cards.

When no microSD card mounts, the library falls back to a read-only FAT
partition in flash (`storage` in `partitions.csv`), built from the `sdcard/`
folder and written with the firmware. The header shows `SD / TAP` or
`FLASH / TAP` for the active source. The board needs 4 MB of flash.

## Board Connections

| Peripheral | GPIO |
| --- | --- |
| ILI9341 MOSI / MISO / clock | 13 / 12 / 14 |
| ILI9341 CS / DC / backlight | 15 / 2 / 21 |
| XPT2046 MOSI / MISO / clock | 32 / 39 / 25 |
| XPT2046 CS / interrupt | 33 / 36 |
| microSD MOSI / MISO / clock / CS | 23 / 19 / 18 / 5 |
| Built-in DAC to onboard speaker amplifier | 26 |

Touch uses a separate software SPI bus, leaving both hardware SPI buses available
for LCD and SD. When no microSD card mounts at startup, touch moves onto the
idle SD SPI host instead (Wokwi's XPT2046 model only answers hardware SPI); a
card inserted later is then not detected until the board restarts. Boards with a
different LCD controller (such as ST7789 variants)
need a different LCD setup.

## Touch Calibration

At startup the firmware reads the microSD card and loads `calibration.json`
from its root. If the file is missing or invalid, or there is no card, it shows
the calibration screen before the file browser: tap the centre of each of three
crosses. The three points give an affine mapping from raw XPT2046 readings
to screen pixels, which covers swapped, mirrored, offset, and slightly rotated
panels. The result is written back to the card:

```json
{
	"version": 1, "width": 320, "height": 240,
	"x": [0, 0.0862, -17.24],
	"y": [-0.0646, 0, 251.9],
	"points": [{"screen_x": 32, "screen_y": 32, "raw_x": 3405, "raw_y": 571}, ...]
}
```

`screen_x = x[0] * raw_x + x[1] * raw_y + x[2]`, and likewise for `y`;
`points` records the raw readings for reference. Delete the file to recalibrate.
Without a card the calibration is used for that session only.

## TAP Audio

`main/tap.c` reads standard Spectrum length-prefixed TAP blocks and checks each
block's XOR checksum before emitting it. It generates the 3.5 MHz Spectrum ROM
timings: 8063 header / 3223 data pilot pulses at 2168 T-states, 667/735 sync
pulses, then two 855/1710 T-state pulses per zero/one bit, MSB first. Each block
ends with a one-second silent pause. TZX, turbo loaders, and other computers'
TAP formats are not supported.

Audio runs on a separate FreeRTOS task through the ESP-IDF continuous DAC driver
(I2S0 DMA), at 250 ksample/s on GPIO26. Fractional sample durations accumulate
across pulses rather than rounding each pulse independently. Only the current
TAP block is held in RAM (at most 65535 bytes); an indexed framebuffer keeps
display memory bounded. Stop requests are checked while filling audio buffers.

The onboard speaker output is for listening. Loading a real Spectrum requires
an appropriately attenuated, AC-coupled audio interface to its EAR input and
hardware verification of the signal level and timing. Do not connect the
amplified speaker output directly to the computer or connect a speaker directly
to the ESP32 DAC pin.

## Simulation

The Wokwi diagram uses Wokwi's `board-esp32-2432s028r` part, a replica of this
board with its ILI9341 display and XPT2046 touchscreen on the same pins. Click
the screen to touch it. Put `.tap` files in the `sdcard/` folder, build the
firmware, then run **Wokwi: Start Simulator**.

The board part does not expose the microSD slot or the speaker, so the
simulated library always reads the flash copy of `sdcard/` (rebuild after
changing the folder) and playback is silent. Actual DAC playback, SD card
mounting, touch calibration, and real Spectrum loading must be checked on
physical hardware.

## Decoder Tests

Run the portable decoder tests on a host with a C11 compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c tests/tap_test.c -o /tmp/tap_test
/tmp/tap_test
```

Tests cover header/data pilot counts, sync and data pulse lengths, MSB-first
bit ordering, multiple blocks, progress, checksum failures, malformed and empty
files, truncated blocks, and cancellation.