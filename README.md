# ESP32-S3 WS2812B Audi-style LED tail light

Dynamic LED tail light for an ESP32-S3 and a WS2812B strip, inspired by the
HAZI TECH "Audi style" tail light video. Built with PlatformIO (Arduino framework) and FastLED.

## Features
- Running light (dim red, brighter in the middle)
- Brake: full bright red
- Reverse: white
- Left / right indicators: amber sweep from the centre outwards
- Hazard: both sides sweep together
- Power-up animation

## Hardware
- ESP32-S3 board (tested config: `4d_systems_esp32s3_gen4_r8n16`)
- WS2812B strip (32 pixels by default, `NUM_LEDS` in `src/main.cpp`)
- 12 V to 5 V buck converter
- 4 voltage dividers (47 kOhm + 10 kOhm, 100 nF) for the 12 V inputs

| Signal  | GPIO |
|---------|------|
| Strip DIN (via 330 ohm) | 5 |
| Brake   | 1 |
| Left    | 2 |
| Right   | 3 |
| Reverse | 4 |

Each 12 V input: car wire -> 47k -> GPIO, GPIO -> 10k -> GND, GPIO -> 100 nF -> GND.
Add a 3.3 V zener/TVS per input and a fuse on the 12 V feed. Do not feed 12 V directly to a GPIO.

## Build and upload
1. Install VS Code and the PlatformIO extension.
2. Open this folder, then Build and Upload.
3. Bench test without a car: open the Serial Monitor (115200) and type
   `b` brake, `l` left, `r` right, `v` reverse, `h` hazard.

## Settings
Edit the `#define` values at the top of `src/main.cpp`
(`NUM_LEDS`, `LEFT_IS_PIXEL_0`, pins, indicator timing, brightness).
