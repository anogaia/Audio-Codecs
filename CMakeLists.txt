cmake_minimum_required(VERSION 3.15)

project(AudioCodecs VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(audio_codecs STATIC
    Codecs/AudioCodec.cpp
    Codecs/Compressors/AudioCodecCompressor_8BitScaled.cpp
    Codecs/Compressors/AudioCodecCompressor_8BitVbrDelta.cpp
    Codecs/Filters/AudioCodecFilter_4X.cpp
    Codecs/Filters/AudioCodecFilter_4X_PolyphaseFIR.cpp
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
)

target_link_libraries(tests PRIVATE audio_codecs)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(tests PRIVATE -fsanitize=address)
    target_link_libraries(tests PRIVATE -fsanitize=address)
endif()

enable_testing()
add_test(NAME AudioCodecsTests COMMAND tests)

