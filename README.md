# DS5Dongle Linux

[中文说明](README.zh-CN.md)

Linux-based DualSense Bluetooth to USB bridge for small USB dongles.

## Overview

This project turns a Bluetooth-connected Sony DualSense controller into a USB
composite device on the host side. The current implementation focuses on macOS:
gamepad input, controller output, speaker/headset playback, microphone capture,
and mic mute handling are handled on the Linux dongle.

## Current Status

This is a working development build.

Tested with:

- Host: macOS 26.1, Apple Silicon Mac mini.
- Dongle OS: Debian GNU/Linux 11 bullseye, aarch64.
- Dongle kernel: Linux 5.15.0-jsbsbxjxh66+ with USB gadget support.
- Controller: Sony DualSense wireless controller over Bluetooth HID.
- USB host mode: composite USB gadget with HID + UAC2 audio.

## Supported Features

- DualSense USB HID identity: `054c:0ce6`, product name `DualSense Wireless Controller`.
- Gamepad input reports to the host at about 250 Hz.
- Button, stick, trigger, touchpad and basic motion input forwarding.
- USB output reports for light bar, player LEDs, rumble and adaptive trigger state.
- USB speaker/headset playback through Bluetooth Opus audio packets.
- USB microphone capture from the DualSense Bluetooth microphone stream.
- Mic mute button handling, including mute state and mic button LED sync.
- DualSense tester 1 kHz speaker/headphone sine-wave test support.
- UAC2 audio function exposed together with the HID gamepad function.

## Known Limitations

- The project is currently tuned and tested mainly on one OpenStick-style
  aarch64 Linux dongle. Other boards may need USB gadget, Bluetooth and ALSA
  adjustments.
- Polling-rate selection is not exposed as a runtime option yet. The current
  USB input send period is 4 ms, about 250 Hz.
- Pico firmware supports configurable polling modes; the Linux gadget path does
  not currently expose HID endpoint `bInterval` as a simple runtime setting.
- Speaker/headset audio over Bluetooth is functional but still experimental.
- The browser-based DualSense tester may send the 1 kHz wave test as normal
  output reports instead of the original feature-report command; this build
  handles that behavior explicitly.

## Build

The project uses CMake and depends on ALSA, pthreads, WDL and Opus.

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j2
```

The output binary is:

```sh
bin/ds5_dongle_linux
```

On Debian-based aarch64 systems, install the required build packages first:

```sh
apt update
apt install -y build-essential cmake pkg-config libasound2-dev
```

## Runtime Requirements

The dongle needs:

- Root access.
- `configfs` mounted at `/sys/kernel/config`.
- USB gadget kernel support: `libcomposite`, `usb_f_hid`, `usb_f_uac2`.
- Bluetooth HID access through BlueZ and `/dev/hidraw*`.
- ALSA PCM device for the UAC2 gadget audio path.

Pair and connect the DualSense through BlueZ first, then run the bridge binary
as root:

```sh
sudo ./bin/ds5_dongle_linux
```

The binary creates the USB composite gadget and forwards data between the
Bluetooth DualSense and the USB host.

## Useful Environment Variables

- `DS5_ENABLE_BT_MIC=1`: enable Bluetooth microphone handling.
- `DS5_BT_MIC_BUTTON_MUTE=1`: make the DualSense mic button toggle mute.
- `DS5_AUDIO_PACKET_MODE=legacy`: use the legacy combined audio packet format.
- `DS5_AUDIO_STATE_MODE=pico-mic`: use the audio state layout currently closest
  to the Pico implementation.
- `DS5_HID_NO_OUT_ENDPOINT=1`: compatibility mode for HID output reports when
  the host sends reports through control transfers.

## Pairing And Connection

Pair and connect the controller with the Linux dongle through BlueZ first.
After the Bluetooth HID device appears, run the bridge binary as root.

## Diagnostics

Useful checks on the dongle:

```sh
ls -l /dev/hidraw* /dev/hidg* /dev/snd/*
bluetoothctl info <controller-mac>
```

Expected runtime signs:

- `/dev/hidraw*` for the Bluetooth DualSense.
- `/dev/hidg0` for the USB HID gadget.
- `/dev/snd/*` entries for UAC2 playback/capture.
- macOS sees `DualSense Wireless Controller` as HID and audio device.

## Credits And References

This Linux build was developed while comparing behavior with the Pico-based
implementation and public DualSense protocol references.

Useful references:

- rafaelvaloto/Pico_W-Dualsense
- egormanga/SAxense
- Paliverse/DualSenseX
- controllers.fandom.com DualSense packet documentation

Original project lineage:

- https://github.com/awalol/DS5Dongle_linux
- https://github.com/awalol/DS5Dongle
