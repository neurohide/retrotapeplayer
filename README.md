# Tape Player

An empty ESP-IDF project for the ESP32-2432S028R (classic ESP32 target).

The application entry point is `main/main.c`. Display, touch, and other board
peripherals are intentionally not configured yet.

## Simulation

The Wokwi configuration approximates the board with an ESP32 DevKitC and an
ILI9341 display wired to the usual CYD SPI pins. It does not emulate the exact
2432S028R board layout or its XPT2046 touch controller. Install the Wokwi VS
Code extension, build the project, and run **Wokwi: Start Simulator** from the
Command Palette. The simulated display stays blank until firmware initializes
it.