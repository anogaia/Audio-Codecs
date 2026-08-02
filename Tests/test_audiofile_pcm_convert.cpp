// Regression tests for AudioSampleConverter 24/32-bit PCM paths when T is int16.
//
// Prior bug: clamp() was typed as T, so SignedInt24/32 limits cast through short
// overflowed to 0/-1 and collapsed non-zero samples to silence.

#include "AudioFile.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

static void expect_true(bool cond, const char *msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_failures++;
    } else {
        printf("PASS: %s\n", msg);
    }
}

static void expect_eq_i32(int32_t got, int32_t want, const char *msg) {
    if (got != want) {
        printf("FAIL: %s (got %d, want %d)\n", msg, got, want);
        g_failures++;
    } else {
        printf("PASS: %s\n", msg);
    }
}

static void expect_eq_i16(int16_t got, int16_t want, const char *msg) {
    if (got != want) {
        printf("FAIL: %s (got %d, want %d)\n", msg, (int)got, (int)want);
        g_failures++;
    } else {
        printf("PASS: %s\n", msg);
    }
}

static void test_converter_24bit_short() {
    printf("\n--- 24-bit <-> int16 converter ---\n");

    // Limits must not truncate through short (old bug: min->0, max->-1).
    expect_eq_i16(AudioSampleConverter<int16_t>::twentyFourBitIntToSample(1000000),
                  static_cast<int16_t>(1000000 >> 8),
                  "24->16 scales 1000000 by >>8 (non-zero)");
    expect_eq_i16(AudioSampleConverter<int16_t>::twentyFourBitIntToSample(-2000000),
                  static_cast<int16_t>(-2000000 >> 8),
                  "24->16 scales negative sample");
    expect_eq_i16(AudioSampleConverter<int16_t>::twentyFourBitIntToSample(8388607),
                  static_cast<int16_t>(32767),
                  "24->16 full-scale positive");
    expect_eq_i16(AudioSampleConverter<int16_t>::twentyFourBitIntToSample(-8388608),
                  static_cast<int16_t>(-32768),
                  "24->16 full-scale negative");

    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToTwentyFourBitInt(16000),
                  16000 << 8,
                  "16->24 scales 16000 by <<8 (not clamped to 0)");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToTwentyFourBitInt(-16000),
                  static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(-16000)) << 8),
                  "16->24 scales negative sample");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToTwentyFourBitInt(32767),
                  32767 << 8,
                  "16->24 full-scale positive");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToTwentyFourBitInt(-32768),
                  static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(-32768)) << 8),
                  "16->24 full-scale negative");

    // Wide integer sample type still stores 24-bit values without shifting.
    expect_eq_i32(AudioSampleConverter<int32_t>::twentyFourBitIntToSample(1000000),
                  1000000,
                  "24->int32 keeps raw 24-bit value");
    expect_eq_i32(AudioSampleConverter<int32_t>::sampleToTwentyFourBitInt(1000000),
                  1000000,
                  "int32->24 keeps value inside 24-bit range");
}

static void test_converter_32bit_short() {
    printf("\n--- 32-bit <-> int16 converter ---\n");

    expect_eq_i16(AudioSampleConverter<int16_t>::thirtyTwoBitIntToSample(
                      static_cast<int32_t>(1000000LL << 16)),
                  static_cast<int16_t>(1000000),
                  "32->16 scales by >>16 (non-zero)");
    expect_eq_i16(AudioSampleConverter<int16_t>::thirtyTwoBitIntToSample(0x7FFF0000),
                  static_cast<int16_t>(0x7FFF),
                  "32->16 near full-scale positive");
    expect_eq_i16(AudioSampleConverter<int16_t>::thirtyTwoBitIntToSample(
                      static_cast<int32_t>(0x80000000u)),
                  static_cast<int16_t>(-32768),
                  "32->16 full-scale negative");

    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToThirtyTwoBitInt(16000),
                  16000 << 16,
                  "16->32 scales 16000 by <<16 (not clamped to 0)");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToThirtyTwoBitInt(-16000),
                  static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(-16000)) << 16),
                  "16->32 scales negative sample");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToThirtyTwoBitInt(32767),
                  32767 << 16,
                  "16->32 full-scale positive");
    expect_eq_i32(AudioSampleConverter<int16_t>::sampleToThirtyTwoBitInt(-32768),
                  static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(-32768)) << 16),
                  "16->32 full-scale negative");
}

static void test_wav_roundtrip_24_32(int bitDepth) {
    printf("\n--- WAV round-trip bitDepth=%d with AudioFile<int16_t> ---\n", bitDepth);

    AudioFile<int16_t> out;
    out.setNumChannels(1);
    out.setNumSamplesPerChannel(4);
    out.setSampleRate(48000);
    out.setBitDepth(bitDepth);
    out.samples[0][0] = 0;
    out.samples[0][1] = 16000;
    out.samples[0][2] = -16000;
    out.samples[0][3] = 32767;

    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("pcm_convert_rt_" + std::to_string(bitDepth) + ".wav"))
            .string();

    expect_true(out.save(path, AudioFileFormat::Wave), "save WAV");

    AudioFile<int16_t> in;
    expect_true(in.load(path), "load WAV into int16");
    expect_true(in.getBitDepth() == bitDepth, "loaded bit depth preserved");
    expect_true(in.getNumSamplesPerChannel() == 4, "sample count");

    // Round-trip through shift scaling should be exact for these values.
    expect_eq_i16(in.samples[0][0], 0, "round-trip sample 0");
    expect_eq_i16(in.samples[0][1], 16000, "round-trip sample 16000 (not silenced)");
    expect_eq_i16(in.samples[0][2], -16000, "round-trip sample -16000");
    expect_eq_i16(in.samples[0][3], 32767, "round-trip sample 32767");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

int main() {
    printf("AudioFile PCM 24/32-bit conversion regression tests\n");

    test_converter_24bit_short();
    test_converter_32bit_short();
    test_wav_roundtrip_24_32(24);
    test_wav_roundtrip_24_32(32);

    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nAll PCM conversion tests passed.\n");
    return 0;
}
