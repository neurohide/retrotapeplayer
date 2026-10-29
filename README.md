# Tape Player

ZX Spectrum TAP / TZX and MSX CAS cassette player for the **ESP32-2432S028R** (classic ESP32,
ILI9341 / XPT2046 "Cheap Yellow Display"), using ESP-IDF 5.4.

## Use

At power-on the board shows the RetroTape Player splash with the app version,
author and build year for at least one second while it reads the SD card. The
version is `PROJECT_VER` in the top-level `CMakeLists.txt`; the author is
`APP_AUTHOR` in `main/main.c`.

1. Format a microSD card as **FAT16 or FAT32** and put ZX Spectrum `.tap` or
	`.tzx` files, or MSX `.cas` files, in its root directory. Without a card the board shows **PLEASE INSERT AN SD
	CARD**; the card can be inserted at any time.
2. Build with **ESP-IDF: Build your project**, then use **ESP-IDF: Flash your
	project**. Connect a speaker to the board's speaker connector.
3. The library lists regular `.tap`, `.tzx` and `.cas` files, each row labelled with
	its type, sorted case-insensitively by
	filename, skipping hidden ones whose names start with `.` (such as the `._`
	files macOS creates). Touch a row to hide the library and start playback immediately.
4. The playback screen shows a photo of a cassette with the selected filename
	above it; each tape started from the library alternates between a TDK D46
	and a Sony HF60 (**REPLAY** keeps the current one). Its reels turn while playing; the progress bar and block number
	update during playback.
5. Touch **PAUSE** to hold the tape where it is (the output goes silent) and
	**RESUME** to continue, the square to stop, the triangle to replay from the
	beginning, or **FILES** to stop and return to the library. Pausing in the
	middle of a block usually makes a Spectrum report a loading error; pause in
	the silent gap after a block, as with a real recorder. Library arrows change pages;
	**RESCAN** retries mounting and refreshes the list. All controls are on the
	touchscreen.

Long filenames are enabled in FATFS. The library holds up to 96 files, five per
page; `96 MAX` indicates additional files were not listed. Subdirectories are
not scanned. Long names scroll on the cassette label during playback. The
pixel font renders ASCII names in uppercase; other characters appear as `?`.
Empty directories, malformed TAP blocks, bad checksums, and read/audio failures
have explicit display states. A failed mount **never formats the card**.

The slot has no card-detect switch, so while nothing is playing the firmware
checks the card about once a second. An inserted card is mounted and its library
shown; a card that answers but holds no FAT16 or FAT32 volume (exFAT, FAT12,
unformatted) shows **CHECK THE SD CARD AND REINSERT** with an alert icon until it
is removed. Pulling the card out returns to the insert screen. Stop playback
before removing a card.

**BUILT-IN TAPES** on either screen opens a read-only FAT partition in flash
(`storage` in `partitions.csv`), built from the `sdcard/` folder and written
with the firmware; inserting a card switches to it. The header shows `SD / TAP TZX CAS`
or `FLASH / TAP TZX CAS` for the active source, and under the title
`LABEL:` with the card's FAT volume label (`NO NAME` if it has none,
`BUILT-IN` for the flash library, `-` without a usable card). The board needs 4 MB of flash.

## Board Connections

| Peripheral | GPIO |
| --- | --- |
| ILI9341 MOSI / MISO / clock | 13 / 12 / 14 |
| ILI9341 CS / DC / backlight | 15 / 2 / 21 |
| XPT2046 MOSI / MISO / clock | 32 / 39 / 25 |
| XPT2046 CS / interrupt | 33 / 36 |
| microSD MOSI / MISO / clock / CS | 23 / 19 / 18 / 5 |
| Built-in DAC to onboard speaker amplifier | 26 |
| Optional FT6206 touch SDA / SCL (CN1 connector) | 27 / 22 |

Touch uses a separate software SPI bus, leaving both hardware SPI buses available
for LCD and SD. At startup the firmware also probes the CN1 I2C pins for an
FT6206 capacitive touch controller (address 0x38) and, when one answers, uses it
instead of the XPT2046; the simulator relies on this. Boards with a different LCD
controller (such as ST7789 variants) need a different LCD setup.

## Touch Calibration

Calibration is tied to the card. Each time a card has been mounted and verified
as FAT16/FAT32, at startup or when inserted later, the firmware applies
`calibration.json` from its root. If the file is missing or invalid, it shows the
calibration screen before the library: tap the centre of each of three crosses. The three points give an affine mapping from raw XPT2046 readings
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
Without a card there is no calibration; the insert screen uses a default
mapping, and its only control is the full-width **BUILT-IN TAPES** bar. With an FT6206
there is no calibration screen: it reports panel pixels, which map to the
screen as `x = 319 - raw_y`, `y = 239 - raw_x` unless `calibration.json` says
otherwise.

## TAP Audio

`main/tap.c` reads standard Spectrum length-prefixed TAP blocks and checks each
block's XOR checksum before emitting it. It generates the 3.5 MHz Spectrum ROM
timings: 8063 header / 3223 data pilot pulses at 2168 T-states, 667/735 sync
pulses, then two 855/1710 T-state pulses per zero/one bit, MSB first. Each block
ends with a one-second silent pause. TZX, turbo loaders, and other computers'
TAP formats are not supported.

`main/tzx.c` plays Spectrum `.tzx` images (format 1.x), streamed so data blocks
of any size work:

| Blocks | Handling |
| --- | --- |
| 0x10 standard, 0x11 turbo, 0x14 pure data | pilot, sync and data pulses as stored; the last byte may use fewer bits |
| 0x12 pure tone, 0x13 pulse sequence | played as stored |
| 0x15 direct recording, 0x18 CSW (RLE) | sampled levels turned into edges |
| 0x19 generalized data | pilot/sync and data symbol tables, including symbols that continue or force the level |
| 0x20 pause | silence; **pause 0 ("stop the tape") pauses the player** with `TAPE STOP - PRESS RESUME`, so press **RESUME** when the program asks you to start the tape |
| 0x23 jump, 0x24/0x25 loop, 0x26/0x27 call sequence | followed |
| 0x2B set signal level | applied |
| 0x21/0x22 group, 0x28 select, 0x2A stop if 48K, 0x30-0x35 and 0x5A info | skipped |

Data checksums are not checked, since protected tapes break them on purpose.
CSW blocks with Z-RLE (zlib) compression stop with `UNSUPPORTED TAPE BLOCK`;
a file without the `ZXTape!` signature or with a truncated block shows
`INVALID TZX FILE`. The output can only toggle, so absolute signal levels
are approximated by their edges, which is what the Spectrum's EAR input reacts
to.

`main/cas.c` plays MSX `.cas` images as the standard 1200 baud MSX tape signal:
a 0 bit is one 1200 Hz cycle, a 1 bit two 2400 Hz cycles, and each byte is a
start bit, eight data bits (least significant first) and two stop bits. Every
block marker (`1F A6 DE BA CC 13 7D 74`) becomes silence plus a 2400 Hz sync
tone: 2 s and 16000 cycles before the first block and before any block that
starts with a file-type header (ten `D0`, `D3` or `EA` bytes), otherwise 1 s and
4000 cycles, as openMSX does. Bytes before the first marker are skipped; a file
without a marker is reported as `INVALID CAS FILE`. The image is streamed, so
CAS files of any size play. Load with `CLOAD`, `BLOAD "CAS:",R` or
`RUN "CAS:"` as the program requires.

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

Wokwi's `board-esp32-2432s028r` part is not usable here: its XPT2046 never
drives MISO or PENIRQ, and its microSD slot is not wired. `diagram.json` instead
rebuilds the board from simulated parts on the same GPIOs: an ESP32 DevKitC V4,
an ILI9341 with FT6206 capacitive touch (on the CN1 pins, see above), and a
microSD card. Put `.tap` files in the `sdcard/` folder, build the firmware, then
run **Wokwi: Start Simulator** and click the screen to touch it.

The simulated card is filled from `sdcard/` at every start; restart the
simulation after changing the folder. Nothing is written back, because Wokwi's
write-back mirrors the card as it was at start and would delete files added to
`sdcard/` while the simulation ran. The simulator starts straight in the library,
since the FT6206 needs no touch calibration.

The simulator has no speaker, and it reports task watchdog warnings for
`tap_audio` during playback. The XPT2046 touch path, actual DAC playback, and
real Spectrum loading must be checked on physical hardware.

## Cassette Photos

The playback screen alternates between two photos:

- `assets/tdk-d46.jpg`: "TDK D46 audio cassette" by Hannes Grobe/AWI,
  [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/), from
  [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:TDK_D46_audio_cassette.jpg).
- `assets/sony-hf60.jpg`: a Sony HF60 cassette. Its source and licence are not
  recorded yet.

`tools/cassette_image.py` crops a photo to the shell, scales it to 256 × 164,
reduces it to 248 colours, and pre-renders 24 rotations of each hub into
`main/cassette-<photo>.bin`, which is embedded in the firmware. Each photo has
its own colours; the firmware swaps them into the display palette when the
cassette changes. To regenerate (macOS `sips` converts the JPEG; the script
needs only Python):

```sh
sips -s format bmp assets/tdk-d46.jpg --out /tmp/tdk-d46.bmp
python3 tools/cassette_image.py tdk-d46 /tmp/tdk-d46.bmp main/cassette-tdk-d46.bin
sips -Z 1920 -s format bmp assets/sony-hf60.jpg --out /tmp/sony-hf60.bmp
python3 tools/cassette_image.py sony-hf60 /tmp/sony-hf60.bmp main/cassette-sony-hf60.bin
```

The same script makes the splash, `main/splash.bin`, from `assets/splash.jpeg`:
it crops the rounded icon, scales it to 200 × 200 and makes its white surround
transparent.

```sh
sips -s format bmp assets/splash.jpeg --out /tmp/splash.bmp
python3 tools/cassette_image.py splash /tmp/splash.bmp main/splash.bin
```

The crop and hub positions in the script's `PHOTOS` table are measured on these
photos at those sizes; another photo needs its own entry, a new `.bin` in
`EMBED_FILES`, and an entry in `cassette_images[]` in `main.c`.

## Decoder Tests

Run the portable decoder tests on a host with a C11 compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c tests/tap_test.c -o /tmp/tap_test
/tmp/tap_test
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c main/tzx.c tests/tzx_test.c -o /tmp/tzx_test
/tmp/tzx_test
cc -std=c11 -Wall -Wextra -Werror -I main main/tap.c main/cas.c tests/cas_test.c -o /tmp/cas_test
/tmp/cas_test
```

Tests cover header/data pilot counts, sync and data pulse lengths, MSB-first
bit ordering, multiple blocks, progress, checksum failures, malformed and empty
files, truncated blocks, and cancellation. TZX tests check that standard blocks
sound exactly like the same TAP file, then cover every sound block type, loops,
jumps, call sequences, stop blocks, level continuation in generalized data,
unsupported CSW compression, invalid files and cancellation. CAS tests cover silence and sync
tone lengths, long versus short headers, bit and byte framing, skipped leading
bytes, files without markers, and cancellation.