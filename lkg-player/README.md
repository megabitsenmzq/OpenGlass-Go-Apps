# lkg-player

Direct DRM/KMS media player for OpenGlass Go.

## Features

- Quilt and spatial photos/videos with preprocessing.
- Plain H.264 playback with NV12 hardware overlay.
- Plain H.265 transcoding to H.264/AAC.
- Playlists, PipeWire audio, pause, looping, and caching.

## Build for Armbian

```sh
sudo apt update
sudo apt install build-essential pkg-config ffmpeg libdrm-dev libgbm-dev libegl-dev libgles-dev libpng-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-pipewire gstreamer1.0-libav pipewire-audio wireplumber \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libjpeg-dev libheif-dev libswscale-dev
make
```

The build requires `../lkg-menu`. The runtime user needs `video`, `render`, and `audio` group access and a running PipeWire session.

## Usage

```sh
# Quilt media
./lkg-player --calibration /path/to/LKG_calibration/visual.json \
  '/path/to/Default Cube_qs8x6.png.json'

# Plain video; no JSON file is required
./lkg-player --calibration /path/to/LKG_calibration/visual.json \
  '/path/to/Plain Video.mp4.json'
```

Videos loop until Ctrl-C. Run `./lkg-player --help` for all options. Calibration defaults to `/run/lkg/visual.json`.

Requires Cedrus, GStreamer `v4l2codecs`, and EGL DMA-BUF support. Use `cma=256M` in the kernel command line.
