# lkg-menu

Direct DRM/KMS menu and gallery for OpenGlass Go. Media playback is handled by `lkg-player`.

## Features

- Gallery with local and USB media, thumbnails, playlists, and preprocessing.
- Wi-Fi, volume, slideshow, 3D menu, and autoplay settings.
- File sharing through dufs and an optional Sway desktop.
- Serial and USB keyboard input.

## Build for Armbian

```sh
apt update
apt install build-essential pkg-config libgbm-dev libegl-dev libgles-dev libdrm-dev \
  libfreetype-dev libpng-dev libjpeg-dev libqrencode-dev libheif-dev fonts-dejavu-core
make
```

Runtime dependencies include dufs, Sway, seatd, PipeWire, WirePlumber, sound-theme-freedesktop, Netplan, and wpa_supplicant.

## Usage

```sh
./lkg-menu
```

Defaults:

- DRM: `/dev/dri/card0`, then `card1`
- Serial: `/dev/ttyS5`
- Font: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
- Media: `/home/lkg/data/media`
- Cache: `/home/lkg/data/cache`
- Calibration: `/run/lkg/visual.json`

Use `--drm-device`, `--serial`, and `--font` to override the corresponding paths.

## Tests

```sh
make test      # Logic tests
make test-ui   # Offscreen UI tests on Debian Trixie
make check     # All tests
```

The full Armbian image and system services are built from `../lkg-armbian`; this repository has no standalone installation flow.
