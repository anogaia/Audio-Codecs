#include "anog_codec_io.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures = 0;

static void expect_true(bool cond, const char *msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_failures++;
    } else {
        printf("PASS: %s\n", msg);
    }
}

static std::string temp_anog_path(const std::string &stem) {
    return (std::filesystem::temp_directory_path() / (stem + ".anog")).string();
}

static void make_tone(std::vector<AnogCodec::Sample> &dst, uint32_t rate, double freq, double seconds) {
    const size_t n = size_t(rate * seconds);
    dst.resize(n);
    for (size_t i = 0; i < n; i++) {
        dst[i] = AnogCodec::Sample(16000.0 * std::sin(2.0 * M_PI * freq * double(i) / double(rate)));
    }
}

static void roundtrip_mono(uint32_t rate, Anog::FilterId expect_filter, uint16_t frame_ms) {
    std::vector<AnogCodec::Sample> mono;
    make_tone(mono, rate, 440.0, 0.5);

    std::vector<std::vector<AnogCodec::Sample>> ch = {mono};
    const std::string path = temp_anog_path("anog_rt_" + std::to_string(rate));

    AnogCodec::encode_wav_channels_to_anog(ch, rate, frame_ms, path);

    Anog::Header hdr;
    std::vector<std::vector<AnogCodec::Sample>> decoded;
    AnogCodec::decode_anog_to_channels(path, hdr, decoded);

    expect_true(hdr.pcm_sample_rate == rate, "pcm rate preserved");
    expect_true(hdr.filter_id == uint8_t(expect_filter), "filter id matches rate");
    expect_true(hdr.channels == 1, "mono channel count");
    expect_true(hdr.frame_ms == frame_ms, "frame_ms preserved");
    expect_true(hdr.seek_entry_count > 0, "seek table non-empty");
    expect_true(decoded.size() == 1 && decoded[0].size() == mono.size(), "decoded length");

    double best_err = 1e100;
    int best_lag = 0;
    const size_t guard = 2000;
    for (int lag = 0; lag < 2000; lag++) {
        double acc = 0.0;
        size_t count = 0;
        for (size_t i = guard; i + size_t(lag) + guard < mono.size(); i++) {
            double d = double(decoded[0][i + size_t(lag)]) - double(mono[i]);
            acc += d * d;
            count++;
        }
        if (count > 0) {
            const double mse = acc / double(count);
            if (mse < best_err) {
                best_err = mse;
                best_lag = lag;
            }
        }
    }
    const double rmse = std::sqrt(best_err);
    printf("  rate=%u frame_ms=%u best_lag=%d rmse=%.1f\n", rate, frame_ms, best_lag, rmse);
    expect_true(rmse < 800.0, "mono round-trip RMSE under threshold (lag-aligned)");
}

static void roundtrip_stereo_48k() {
    std::vector<AnogCodec::Sample> left, right;
    make_tone(left, 48000, 440.0, 0.3);
    make_tone(right, 48000, 660.0, 0.3);
    std::vector<std::vector<AnogCodec::Sample>> ch = {left, right};
    const std::string path = temp_anog_path("anog_rt_stereo");
    AnogCodec::encode_wav_channels_to_anog(ch, 48000, 100, path);

    Anog::Header hdr;
    std::vector<std::vector<AnogCodec::Sample>> decoded;
    AnogCodec::decode_anog_to_channels(path, hdr, decoded);
    expect_true(hdr.channels == 2, "stereo channel count");
    expect_true(decoded.size() == 2 && decoded[0].size() == left.size(), "stereo decoded size");
}

int main() {
    printf("=== ANOG round-trip tests ===\n");
    try {
        roundtrip_mono(48000, Anog::Filter_48k_4X_Polyphase, 100);
        roundtrip_mono(44100, Anog::Filter_44100_12000_Polyphase, 100);
        roundtrip_stereo_48k();
    } catch (const std::exception &ex) {
        printf("FAIL: exception: %s\n", ex.what());
        return 1;
    }
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("All ANOG round-trip tests passed.\n");
    return 0;
}
