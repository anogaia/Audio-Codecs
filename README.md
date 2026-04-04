# Audio Codecs

A small C++ audio codec library and test harness built with modern CMake.

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

If the test target is available through CTest:

```bash
ctest --output-on-failure
```

## Development

- The project targets **C++17**.
- The main library target is `audio_codecs`.
- The test executable is `tests` and links against `audio_codecs`.

### Project Structure

- `Codecs/AudioCodec.cpp` and `Codecs/AudioCodec.hpp`
- `Codecs/Compressors/`
- `Codecs/Filters/`
- `Codecs/Squelchers/`
- `Codecs/Utility/`
- `Tests/tests.cpp`

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/my-change`
3. Commit your changes with clear messages.
4. Push your branch and open a pull request.

Please keep contributions focused, provide tests for new behavior, and ensure the project still builds cleanly.

## License

This project is licensed under the GNU General Public License v3.0. See `LICENSE` for details.
