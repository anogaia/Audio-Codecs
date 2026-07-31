# Audio Codecs

A small C++ audio codec library and test harness built with modern CMake.

This codec is designed for VOIP-style use cases, delivering near 16-bit perceptual audio quality at low bit rates around 8 kB/s (64 kbps).

## Overview

This repository contains:

- `Codecs/`: core audio codec implementation files
- `Tests/`: test executable and test audio generation utilities
- `tools/`: `anog` CLI converter, plus offline FIR design scripts
- `docs/`: design notes (`.anog` format, 44.1↔12 kHz filter, …)
- `CMakeLists.txt`: modern CMake build script for a static library and test targets

## Build Instructions

1. Create a build directory:

```bash
mkdir -p build && cd build
```

2. Configure the project with CMake:

```bash
cmake ..
```

3. Build the library and tests:

```bash
cmake --build .
```

## `.anog` files

Compressed audio uses the **ANOG** container (extension `.anog`): header + seek table + length-prefixed interleaved mono packets. Spec: [`docs/ANOG_FORMAT.md`](docs/ANOG_FORMAT.md).

```bash
./anog -enc input.wav [output.anog] [--frame-ms 100]
./anog -dec input.anog [output.wav]
./anog -enc '*'                  # all WAVs in cwd → .anog beside them
./anog -dec '*'                  # all ANOGs in cwd → .wav beside them
./anog -all                      # all WAVs in cwd → ./ANOG/*.anog
./anog -all -r                   # recurse; mirror tree under ./ANOG/
```

Quote `'*'` so the shell does not expand it. Existing outputs prompt before overwrite. `--frame-ms` sets encode frame length (default **100**). Supported PCM rates: **48000** (4:1 filter) and **44100** (40:147 filter). Stereo is two mono streams interleaved per frame.

Original vs decoded residual distortion (THD+N-style):

```bash
source tools/.venv/bin/activate
python tools/anog_thd_compare.py [original.wav] [decoded.wav]
```

## Run Tests

From the `build/` directory:

```bash
./tests
./test_filter_44100_12000
./test_anog_roundtrip
```

Or via CTest:

```bash
ctest --output-on-failure
```

Related docs: [`docs/FILTER_44100_12000.md`](docs/FILTER_44100_12000.md) · [`docs/ANOG_FORMAT.md`](docs/ANOG_FORMAT.md)

## Development

- The project targets **C++17**.
- The main library target is `audio_codecs`.
- CLI tool `anog` links against `audio_codecs`.

### Project Structure

- `Codecs/AudioCodec.cpp` and `Codecs/AudioCodec.hpp`
- `Codecs/Compressors/`
- `Codecs/Filters/`
- `Codecs/Squelchers/`
- `Codecs/Utility/` (`AnogFile.hpp`, …)
- `tools/anog.cpp`
- `Tests/tests.cpp`

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/my-change`
3. Commit your changes with clear messages.
4. Push your branch and open a pull request.

Please keep contributions focused, provide tests for new behavior, and ensure the project still builds cleanly.

## License

This project is licensed under the GNU General Public License v3.0. See `LICENSE` for details.
