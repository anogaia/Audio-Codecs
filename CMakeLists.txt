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
    foreach(tgt tests test_filter_44100_12000 test_anog_roundtrip anog)
        target_compile_options(${tgt} PRIVATE -fsanitize=address)
        target_link_libraries(${tgt} PRIVATE -fsanitize=address)
    endforeach()
endif()

enable_testing()
add_test(NAME AudioCodecsTests COMMAND tests)
add_test(NAME Filter44100To12000 COMMAND test_filter_44100_12000)
add_test(NAME AnogRoundTrip COMMAND test_anog_roundtrip)
