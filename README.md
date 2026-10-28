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
	**RESCAN** retries mounting and refreshes the list. The board's BOOT button
	also stops playback and returns to the library.

Long filenames are enabled in FATFS. The library holds up to 96 files, five per
page; `96 MAX` indicates additional files were not listed. Subdirectories are
not scanned. Long names scroll on the cassette label during playback. The
pixel font renders ASCII names in uppercase; other characters appear as `?`.
Missing cards, empty directories, malformed TAP blocks, bad checksums, and
read/audio failures have explicit display states. A failed mount **never
formats the card**. Stop playback before removing a card; restart the board
after swapping cards.

## Board Connections

| Peripheral | GPIO |
| --- | --- |
| ILI9341 MOSI / MISO / clock | 13 / 12 / 14 |
| ILI9341 CS / DC / backlight | 15 / 2 / 21 |
| XPT2046 MOSI / MISO / clock | 32 / 39 / 25 |
| XPT2046 CS / interrupt | 33 / 36 |
| microSD MOSI / MISO / clock / CS | 23 / 19 / 18 / 5 |
| Built-in DAC to onboard speaker amplifier | 26 |
| Optional next / play-stop / files buttons | 27 / 22 / 0 |

Optional buttons are active-low, connected between GPIO and ground. GPIO0 is
the onboard BOOT button; do not hold it while resetting. Touch uses a separate
software SPI bus, leaving both hardware SPI buses available for LCD and SD.
Touch calibration defaults to raw coordinates 200..3900 with landscape axis
swapping. If a board revision has reversed or offset touch coordinates, adjust
`TOUCH_MIN`, `TOUCH_MAX`, and `touch_position()` in `main/main.c`. Boards with a
different LCD controller (such as ST7789 variants) need a different LCD setup.

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

The Wokwi diagram approximates the board with an ESP32 DevKitC, ILI9341, SPI
microSD card, buzzer, and three buttons. Build the firmware, then run **Wokwi:
Start Simulator**. With Wokwi for VS Code 4.0.0 or later, put `.tap` files in
the local `sdcard/` folder and restart the simulation to load them onto the
virtual card. Folder-backed cards are FAT16, also supported by the firmware.
To test FAT32 specifically, use a FAT32 raw disk image with an `[[sdcard]]`
section and `image = 'sd.img'` in `wokwi.toml` instead.

The touchscreen is not emulated, so use the diagram buttons: **NEXT** selects
the next file, **PLAY / STOP** opens it or stops/replays the cassette, and
**FILES** returns to the library. On an empty library, **PLAY / STOP** rescans.
The diagram also binds these buttons to N, P, and B. The buzzer only approximates
the speaker; actual DAC playback, touch calibration, and real Spectrum loading
must be checked on physical hardware.

## Decoder Tests

Run the portable decoder tests on a host with a C11 compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c tests/tap_test.c -o /tmp/tap_test
/tmp/tap_test
```

Tests cover header/data pilot counts, sync and data pulse lengths, MSB-first
bit ordering, multiple blocks, progress, checksum failures, malformed and empty
files, truncated blocks, and cancellation.