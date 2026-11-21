# lkg-mcu

Builds `lkg-mcu`, a small serial utility for the internal STM32 MCU.

## Features

- Read the device serial number and calibration values.
- Generate `visual.json` from a template.
- Notify the MCU that Linux is online.
- Handle MCU power-off and USB/standalone mode switching.

## Build

```sh
make
```

The output binary is `lkg-mcu`. Only a C compiler and standard POSIX libc are required.

## Usage

Generate a calibration file:

```sh
./lkg-mcu init \
  --template /etc/lkg/visual-template.json \
  --output /run/lkg/visual.json
```

Print the generated JSON instead:

```sh
./lkg-mcu init \
  --template /etc/lkg/visual-template.json \
  --print
```

Other commands:

```sh
./lkg-mcu poweroff
./lkg-mcu watch
```

All commands use `/dev/ttyS5` by default. Add `--device /dev/ttySX` to select another serial port.

`init` replaces the template's `serial`, `pitch`, `slope`, and `center` values and writes the output atomically. `poweroff` checks the MCU state before requesting power off. `watch` is intended for the USB mode watcher service.

The complete image, templates, and systemd services are built from `../lkg-armbian`.
