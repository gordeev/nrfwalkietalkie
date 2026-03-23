# Flipper Zero NRF Walkie

`Flipper Zero` external app (`.fap`) that mirrors the `walkie_oled.ino` idea:

- external `nRF24L01+`
- external analog microphone module like `MAX9814`
- external analog speaker amplifier and speaker
- `PTT` on the Flipper `OK` button
- `NODE_ID` selection inside the app with `Left/Right`

This project is written as a standalone app folder so you can copy it into a Flipper firmware tree under `applications_user/`.

## What This App Matches

By default this app follows the packet format from `walkie_oled.ino`:

- half-duplex `PTT`
- `8 kHz`, `8-bit PCM`
- `24` audio bytes per packet
- `seq + nonce + pcm[24]`
- `nRF24` channel `90`
- `250 kbps`
- optional simple XOR obfuscation

Important:

- `walkie_oled.ino` uses the XOR option by default
- `walkie_node0.ino` / `walkie_node1.ino` use the simpler no-crypto packet format
- this Flipper app is currently configured for the `walkie_oled.ino` packet format

If you want to talk to `walkie_node0.ino` / `walkie_node1.ino`, you will need to:

1. set `WALKIE_ENABLE_SIMPLE_CRYPT` to `0` in `walkie_protocol.h`
2. remove `nonce` from the packet definition to match the simple pair

## Why Software SPI Is Used

On `Flipper Zero`, the convenient PWM audio output pin conflicts with the default external hardware `SPI` pin set.

Because of that, this app uses:

- `software SPI` for `nRF24`
- dedicated PWM output on `PA7` for speaker audio

That keeps the wiring compact and still leaves a clean audio output path.

## Wiring

### Flipper Pins Used

- `PA7` (`GPIO pin 2`) -> audio PWM output to amplifier input
- `PA6` (`GPIO pin 3`) -> `nRF24 MISO`
- `PB3` (`GPIO pin 5`) -> `nRF24 SCK`
- `PB2` (`GPIO pin 6`) -> `nRF24 CE`
- `PC3` (`GPIO pin 7`) -> microphone analog output
- `PC1` (`GPIO pin 15`) -> `nRF24 MOSI`
- `PC0` (`GPIO pin 16`) -> `nRF24 CSN`
- `3.3V` (`GPIO pin 9`) -> `nRF24 VCC`, microphone VCC if it is a `3.3V` board
- `GND` -> all module grounds together

### nRF24L01+

- `VCC` -> `3.3V`
- `GND` -> `GND`
- `CE` -> `PB2`
- `CSN` -> `PC0`
- `SCK` -> `PB3`
- `MISO` -> `PA6`
- `MOSI` -> `PC1`

Strongly recommended:

- place `470uF + 100nF` between `3.3V` and `GND` near the `nRF24`
- especially important for `PA/LNA` modules

### Microphone Module (`MAX9814` or Similar Analog Module)

- `VCC` -> `3.3V`
- `GND` -> `GND`
- `OUT` -> `PC3`

Use an analog-output module here.

### Speaker / Amplifier

The app outputs PWM audio on `PA7`, so use the same simple output chain idea as in the ESP sketch:

- `PA7` -> `1k` resistor -> audio node
- audio node -> `100nF` to `GND`
- audio node -> `1uF` series capacitor -> amplifier input
- `GND` of Flipper -> amplifier `GND`
- amplifier output -> speaker

Notes:

- if your amplifier can run from `3.3V`, power it from Flipper `3.3V`
- if your amplifier needs `5V`, use Flipper `5V` on GPIO and enable `5V on GPIO` in the GPIO app first
- do not power `nRF24` from `5V`

## Controls

- `OK hold` -> push-to-talk
- `OK release` -> return to receive mode
- `Left/Right` -> toggle `NODE_ID` between `0` and `1`
- `Back` -> exit

## On-Screen Layout

The app currently draws:

- current `NODE_ID`
- current mode `TX` / `RX`
- `PTT` state
- packet counters
- RX buffer fill level
- a placeholder `ART TBD` box for future pixel art

## Build

This folder is meant to live inside a Flipper firmware repo as an external app.

## Build On Unleashed Firmware

This flow was verified as a practical way to build the app for `Unleashed`.

### 1. Clone Unleashed Firmware

```sh
git clone --recursive https://github.com/DarkFlippers/unleashed-firmware.git
cd unleashed-firmware
```

If you already cloned it without `--recursive`, run:

```sh
git submodule update --init --recursive
```

### 2. Copy This App Into `applications_user`

Copy this project folder:

`/Users/georgygordeev/Git/private/nrfwalkitalkie/flipper_walkie_fap`

into:

`unleashed-firmware/applications_user/flipper_walkie_fap`

### 3. Build The App

From the root of `unleashed-firmware`:

```sh
./fbt build APPSRC=applications_user/flipper_walkie_fap
```

Or by app id:

```sh
./fbt fap_flipper_walkie
```

### 4. Launch Directly On Flipper

If the Flipper is connected over USB:

```sh
./fbt launch APPSRC=applications_user/flipper_walkie_fap
```

For `Unleashed`, this variant is also often useful:

```sh
./fbt COMPACT=1 DEBUG=0 launch_app APPSRC=applications_user/flipper_walkie_fap
```

### 5. Where The Built `.fap` Usually Appears

Usually the built file ends up somewhere under `build/...` or `dist/...` inside the `unleashed-firmware` tree.

If you want a more convenient packaged output, run:

```sh
./fbt fap_dist
```

Then the built `fap` files are usually easier to find in `dist`.

### Option A: Copy The Folder

Copy `flipper_walkie_fap` into your Flipper firmware checkout:

`applications_user/flipper_walkie_fap`

Then build:

```sh
./fbt build APPSRC=applications_user/flipper_walkie_fap
```

### Option B: Launch Directly To Flipper

```sh
./fbt launch APPSRC=applications_user/flipper_walkie_fap
```

### Option C: Build By App ID

```sh
./fbt fap_flipper_walkie
```

That assumes the app is already inside the firmware tree.

## Install Without Reflashing Full Firmware

You do not need to fully reflash Flipper just to use this app.

Typical flow:

1. build the `.fap`
2. copy the built `.fap` to the SD card
3. open it from the Flipper apps menu

As always, the app must be built against a firmware SDK/API compatible with the firmware currently installed on the device.

## Practical Notes

- This is a first-pass app intended to be realistic and hackable, not studio-quality audio.
- Audio timing is done with polling and PWM updates, so quality will depend on the exact firmware build and external analog stage.
- The radio code uses bit-banged SPI to preserve the dedicated audio PWM pin.
- If you hear only noise when pairing with ESP, first confirm:
  - same `NODE_ID` direction on both ends
  - same radio channel and bitrate
  - same packet format
  - same crypto setting

## Files

- `application.fam` - Flipper app manifest
- `flipper_walkie.c` - UI, app lifecycle, audio/radio worker
- `nrf24_soft.c` / `nrf24_soft.h` - software SPI `nRF24` driver
- `walkie_protocol.h` - packet format and crypto helper

## Next Good Steps

Once the first radio link is alive, the next improvements worth doing are:

1. add a real settings screen instead of only `Left/Right` toggle
2. add saved settings for last `NODE_ID`
3. add a small level meter for mic / RX buffer
4. replace the placeholder art box with your dolphin-and-radio pixel art
