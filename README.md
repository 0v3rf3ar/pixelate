# Pixelate

[![Language: C](https://img.shields.io/badge/language-C-00599C?logo=c&logoColor=white)](https://github.com/0v3rf3ar/pixelate)
[![Release](https://img.shields.io/github/v/release/0v3rf3ar/pixelate?display_name=release&logo=github)](https://github.com/0v3rf3ar/pixelate/releases/latest)
[![Release downloads](https://img.shields.io/github/downloads/0v3rf3ar/pixelate/total?logo=github&label=release%20downloads)](https://github.com/0v3rf3ar/pixelate/releases)
[![License: Proprietary](https://img.shields.io/badge/license-proprietary-blue?logo=readthedocs&logoColor=white)](LICENSE)
[![Repository](https://img.shields.io/badge/repository-0v3rf3ar%2Fpixelate-181717?logo=github)](https://github.com/0v3rf3ar/pixelate)
[![Build binaries](https://github.com/0v3rf3ar/pixelate/actions/workflows/build.yml/badge.svg)](https://github.com/0v3rf3ar/pixelate/actions/workflows/build.yml)

Pixelate renders images and videos as ASCII or colored blocks directly in a
terminal. It supports Linux, macOS, and Windows terminals with 24-bit ANSI
color.

## Features

- Render common image formats as plain ASCII, true-color ASCII, or color blocks.
- Encode videos and optional audio to Pixelate's compact `.asv` format with
  FFmpeg.
- Play `.asv` files with pause, seek, live terminal resizing, and graceful
  Escape or Ctrl+C handling.
- Preserve aspect ratio automatically or fill the available terminal area.
- Run on Linux, macOS, and Windows from one C99 codebase.

## Download

Download the latest prebuilt binary for your system:

| System | Architecture | Download |
| --- | --- | --- |
| Linux | x86-64 | [pixelate-linux-amd64.zip](https://github.com/0v3rf3ar/pixelate/releases/latest/download/pixelate-linux-amd64.zip) |
| Linux | ARM64 | [pixelate-linux-arm64.zip](https://github.com/0v3rf3ar/pixelate/releases/latest/download/pixelate-linux-arm64.zip) |
| Windows | x86-64 | [pixelate-windows-amd64.zip](https://github.com/0v3rf3ar/pixelate/releases/latest/download/pixelate-windows-amd64.zip) |
| macOS | Apple silicon | [pixelate-macos-silicon.zip](https://github.com/0v3rf3ar/pixelate/releases/latest/download/pixelate-macos-silicon.zip) |

For other systems and architectures, follow the build instructions below.

## Build

You need a C99 compiler and CMake 3.16 or newer. FFmpeg is also required when
encoding videos.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable is `build/pixelate` on Linux and macOS. With a multi-config
Windows generator, it is normally `build/Release/pixelate.exe`.

## Usage

```sh
# Render an image using the largest aspect-preserving size
./build/pixelate image photo.png --size auto

# Render with source colors or solid color blocks
./build/pixelate image photo.png --size auto --color
./build/pixelate image photo.png --size fit --block

# Encode and play a video
./build/pixelate video encode input.mp4 output.asv --size auto --fps 24 --audio
./build/pixelate video play output.asv --color --size auto
./build/pixelate video play output.asv --block --size fit
```

Run `pixelate --help` for the complete command reference. Run
`pixelate --version` to see the version, developer, and project address.

### Display options

- `-c`, `--color`: use each source pixel's color on its ASCII character.
- `-b`, `--block`: draw solid, source-colored background blocks.
- `-i`, `--invert`: reverse the ASCII brightness ramp, or invert RGB colors in
  block mode.
- `--remove <chars>`: replace the listed ASCII characters with spaces.

### Video options

- `-a`, `--audio`: include the source audio when encoding an ASV file.
- `--fps <12-30>`: set the encoded video frame rate.

### Sizing

- `--size auto`: use the largest size that preserves the source aspect ratio.
- `--size fit`: fill the terminal; video playback follows terminal resizing.
- `--size WxH`: set the output image or encoded ASV frame dimensions.
- `--size native`: play an ASV file at its stored dimensions.

Audio-enabled ASV3 files play their embedded audio automatically and require
`ffplay` at playback time. Press Space to pause or resume, and use Left or Right
to seek five seconds; audio follows all playback controls. Use Up and Down to
adjust audio through 10 volume levels. Press Escape or Ctrl+C to exit
gracefully. ASV1 and ASV2 files remain fully supported.

## Credits

Pixelate uses [stb_image](https://github.com/nothings/stb) by Sean Barrett and
contributors.

## License

Copyright (c) 2026 s3p. All rights reserved. This is proprietary software; see
[LICENSE](LICENSE) for the restrictions that apply.
