# Pixelate

Pixelate renders images and videos as ASCII or colored blocks directly in a
terminal. It supports Linux, macOS, and Windows terminals with 24-bit ANSI
color.

## Features

- Render common image formats as plain ASCII, true-color ASCII, or color blocks.
- Encode videos to Pixelate's compact `.asv` format with FFmpeg.
- Play `.asv` files with pause, seek, live terminal resizing, and graceful
  Ctrl+C handling.
- Preserve aspect ratio automatically or fill the available terminal area.
- Run on Linux, macOS, and Windows from one C99 codebase.

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
./build/pixelate video encode input.mp4 output.asv --size auto --fps 24
./build/pixelate video play output.asv --color --size auto
./build/pixelate video play output.asv --block --size fit
```

Run `pixelate --help` for the complete command reference. Run
`pixelate --version` to see the version, developer, and project address.

### Display options

- `-c`, `--color`: use each source pixel's color on its ASCII character.
- `-b`, `--block`: draw solid, source-colored background blocks.
- `-i`, `--invert`: reverse the ASCII brightness ramp.
- `--remove <chars>`: replace the listed ASCII characters with spaces.

### Sizing

- `--size auto`: use the largest size that preserves the source aspect ratio.
- `--size fit`: fill the terminal; video playback follows terminal resizing.
- `--size WxH`: set the image sample block or encoded ASV frame dimensions.
- `--size native`: play an ASV file at its stored dimensions.

Video encoding accepts frame rates from 12 to 30 FPS. ASV playback has no
audio; press Space to pause or resume, and use Left or Right to seek five
seconds.

## Credits

Pixelate uses [stb_image](https://github.com/nothings/stb) by Sean Barrett and
contributors.

## License

Copyright (c) 2026 s3p. All rights reserved. This is proprietary software; see
[LICENSE](LICENSE) for the restrictions that apply.
