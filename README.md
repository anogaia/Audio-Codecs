# Audio Codecs

A small C++ audio codec library and test harness built with modern CMake.

This codec is designed for VOIP-style use cases, delivering near 16-bit perceptual audio quality at low bit rates around 8 kB/s (64 kbps).

## Overview

This repository contains:

- `Codecs/`: core audio codec implementation files
- `Tests/`: test executable and test audio generation utilities
- `CMakeLists.txt`: modern CMake build script for a static library and test target

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

## Run Tests

From the `build/` directory:

```bash
./tests
```

Sibling harnesses (leave the default `tests` path unchanged):

```bash
./tests_hd       # HD path smoke + residual metrics (2X FIR + 12-bit VBR)
./tests_listen   # A/B listening compare: legacy 8-bit/12 kHz vs HD 12-bit/24 kHz
```

`tests_listen` writes same-rate/bit-depth decoded WAVs (plus residuals and a short summary) under `Tests/TestData/ListeningCompare/` for side-by-side audition.

If the test target is available through CTest:

```bash
ctest --output-on-failure
```

## Development

- The project targets **C++17**.
- The main library target is `audio_codecs`.
- The test executables are `tests`, `tests_hd`, and `tests_listen`; each links against `audio_codecs`.

### Project Structure

- `Codecs/AudioCodec.cpp` and `Codecs/AudioCodec.hpp`
- `Codecs/Compressors/`
- `Codecs/Filters/`
- `Codecs/Squelchers/`
- `Codecs/Utility/`
- `Tests/tests.cpp`
- `Tests/tests_hd.cpp`
- `Tests/tests_listen.cpp`

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/my-change`
3. Commit your changes with clear messages.
4. Push your branch and open a pull request.

Please keep contributions focused, provide tests for new behavior, and ensure the project still builds cleanly.

## License

This project is licensed under the GNU General Public License v3.0. See `LICENSE` for details.
