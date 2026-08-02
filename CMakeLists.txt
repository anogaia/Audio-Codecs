cmake_minimum_required(VERSION 3.15)

project(AudioCodecs VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(audio_codecs STATIC
    Codecs/AudioCodec.cpp
    Codecs/Compressors/AudioCodecCompressor_8BitScaled.cpp
    Codecs/Compressors/AudioCodecCompressor_8BitVbrDelta.cpp
    Codecs/Compressors/AudioCodecCompressor_12BitScaled.cpp
    Codecs/Compressors/AudioCodecCompressor_12BitVbrDelta.cpp
    Codecs/Filters/AudioCodecFilter_4X.cpp
    Codecs/Filters/AudioCodecFilter_4X_PolyphaseFIR.cpp
    Codecs/Filters/AudioCodecFilter_2X_PolyphaseFIR.cpp
    Codecs/Filters/AudioCodecFilter_44100_12000_PolyphaseFIR.cpp
    Codecs/Squelchers/AudioCodecSquelcher_Basic.cpp
)

target_include_directories(audio_codecs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
        ${CMAKE_CURRENT_SOURCE_DIR}/Codecs/Compressors
        ${CMAKE_CURRENT_SOURCE_DIR}/Codecs/Filters
        ${CMAKE_CURRENT_SOURCE_DIR}/Codecs/Squelchers
        ${CMAKE_CURRENT_SOURCE_DIR}/Codecs/Utility
)

target_compile_features(audio_codecs PUBLIC cxx_std_17)

add_executable(tests
    Tests/tests.cpp
)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(tests PRIVATE audio_codecs)

# Sibling HD harness: 2X polyphase FIR + 12-bit VBR on speech WAVs (does not replace `tests`).
add_executable(tests_hd
    Tests/tests_hd.cpp
)

target_include_directories(tests_hd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

target_link_libraries(tests_hd PRIVATE audio_codecs)

# Sibling listening A/B harness: legacy 8-bit/12 kHz vs HD 12-bit/24 kHz on speech WAVs.
add_executable(tests_listen
    Tests/tests_listen.cpp
)

target_include_directories(tests_listen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

target_link_libraries(tests_listen PRIVATE audio_codecs)

# Optional: also build a tiny sine smoke path into the default `tests` binary.
option(AUDIO_CODECS_TEST_HD "Build an optional HD (2X + 12-bit) smoke test into the tests binary" OFF)
if(AUDIO_CODECS_TEST_HD)
    target_compile_definitions(tests PRIVATE AUDIO_CODECS_TEST_HD)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(tests PRIVATE -fsanitize=address)
    target_link_libraries(tests PRIVATE -fsanitize=address)
    target_compile_options(tests_hd PRIVATE -fsanitize=address)
    target_link_libraries(tests_hd PRIVATE -fsanitize=address)
    target_compile_options(tests_listen PRIVATE -fsanitize=address)
    target_link_libraries(tests_listen PRIVATE -fsanitize=address)
endif()

add_executable(test_filter_44100_12000
    Tests/test_filter_44100_12000.cpp
)

target_include_directories(test_filter_44100_12000 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
)

target_link_libraries(test_filter_44100_12000 PRIVATE audio_codecs)

add_executable(test_anog_roundtrip
    Tests/test_anog_roundtrip.cpp
)

target_include_directories(test_anog_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(test_anog_roundtrip PRIVATE audio_codecs)

add_executable(test_audiofile_pcm_convert
    Tests/test_audiofile_pcm_convert.cpp
)

target_include_directories(test_audiofile_pcm_convert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

add_executable(anog
    tools/anog.cpp
)

target_include_directories(anog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Codecs
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_link_libraries(anog PRIVATE audio_codecs)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    foreach(tgt test_filter_44100_12000 test_anog_roundtrip test_audiofile_pcm_convert anog)
        target_compile_options(${tgt} PRIVATE -fsanitize=address)
        target_link_libraries(${tgt} PRIVATE -fsanitize=address)
    endforeach()
endif()

enable_testing()
add_test(NAME AudioCodecsTests COMMAND tests)
add_test(NAME AudioCodecsHdTests COMMAND tests_hd)
add_test(NAME AudioCodecsListenCompare COMMAND tests_listen)

add_test(NAME Filter44100To12000 COMMAND test_filter_44100_12000)
add_test(NAME AnogRoundTrip COMMAND test_anog_roundtrip)
add_test(NAME AudioFilePcmConvert COMMAND test_audiofile_pcm_convert)
