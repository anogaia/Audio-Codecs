#include "../Codecs/AudioCodec.hpp"
#include "../Codecs/Filters/AudioCodecFilter_44100_12000_PolyphaseFIR.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Sample = AudioF32;

static int g_failures = 0;

static void expect_true(bool cond, const char *msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_failures++;
    } else {
        printf("PASS: %s\n", msg);
    }
}

// Steady-state rate over a long contiguous stream (multiple packets).
static void test_rate_ratio() {
    AudioCodecFilter_44100_12000_PolyphaseFIR<Sample> filt;
    filt.reset();

    const uint32_t packet = 1024;
    const int num_packets = 147;  // many packets; total inputs = 1024*147
    uint64_t total_in = 0;
    uint64_t total_out = 0;

    std::vector<Sample> in(packet, 0.25f);
    std::vector<Sample> out;
    out.resize(packet);  // oversized; actual size queried each time

    bool counts_ok = true;
    for (int p = 0; p < num_packets; p++) {
        uint32_t n_out = filt.getDecimatedSamples(packet);
        out.resize(n_out);
        uint32_t written = n_out;
        filt.decimate(in.data(), packet, out.data(), &written);
        if (written != n_out) {
            counts_ok = false;
            printf("  packet %d expected %u wrote %u\n", p, n_out, written);
            break;
        }
        total_in += packet;
        total_out += written;
    }
    expect_true(counts_ok, "decimate wrote getDecimatedSamples count on all packets");

    const double expected = (double)total_in * 40.0 / 147.0;
    const double err = std::fabs((double)total_out - expected);
    printf("Rate ratio: in=%llu out=%llu expected=%.3f err=%.3f\n",
           (unsigned long long)total_in, (unsigned long long)total_out, expected, err);
    // Exact rational: after enough samples, count must match floor/ceil within 1,
    // and for exact multiples of 147 inputs aligned from reset, equal to n*40/147.
    expect_true(err < 1.0 + 1e-9, "long-run output count within 1 of n*40/147");

    // Exact check from reset with multiple of 147 inputs
    filt.reset();
    const uint32_t exact_in = 147 * 20;
    std::vector<Sample> in2(exact_in, 0.1f);
    uint32_t expect_out = filt.getDecimatedSamples(exact_in);
    std::vector<Sample> out2(expect_out);
    uint32_t written2 = expect_out;
    filt.decimate(in2.data(), exact_in, out2.data(), &written2);
    printf("Exact block: in=%u out=%u (expect %u)\n", exact_in, written2, exact_in * 40 / 147);
    expect_true(written2 == exact_in * 40 / 147, "147k inputs produce exact 40k outputs");
    expect_true(written2 == expect_out, "getDecimatedSamples matches exact block");
}

static void test_expand_rate() {
    AudioCodecFilter_44100_12000_PolyphaseFIR<Sample> filt;
    filt.reset();

    const uint32_t exact_in = 40 * 20;  // low-rate samples
    std::vector<Sample> in(exact_in, 0.1f);
    uint32_t expect_out = filt.getExpandedSamples(exact_in);
    std::vector<Sample> out(expect_out);
    uint32_t written = expect_out;
    filt.expand(in.data(), exact_in, out.data(), &written);
    printf("Expand exact: in=%u out=%u (expect %u)\n", exact_in, written, exact_in * 147 / 40);
    expect_true(written == exact_in * 147 / 40, "40k low-rate inputs produce exact 147k outputs");
    expect_true(written == expect_out, "getExpandedSamples matches exact block");
}

static void test_roundtrip_tone() {
    AudioCodecFilter_44100_12000_PolyphaseFIR<Sample> filt;
    filt.reset();

    const double fs = 44100.0;
    const double f0 = 1000.0;
    const uint32_t n_in = 147 * 80;  // ~0.27 s
    std::vector<Sample> original(n_in);
    for (uint32_t i = 0; i < n_in; i++) {
        original[i] = (Sample)(0.5 * std::sin(2.0 * M_PI * f0 * (double)i / fs));
    }

    uint32_t n_mid = filt.getDecimatedSamples(n_in);
    std::vector<Sample> mid(n_mid);
    uint32_t written_mid = n_mid;
    filt.decimate(original.data(), n_in, mid.data(), &written_mid);
    expect_true(written_mid == n_mid, "roundtrip mid count");

    // Expand side uses separate state; reset only decode path via full reset would
    // clear encode too — expand has its own state, already at 0.
    uint32_t n_out = filt.getExpandedSamples(written_mid);
    std::vector<Sample> recovered(n_out);
    uint32_t written_out = n_out;
    filt.expand(mid.data(), written_mid, recovered.data(), &written_out);
    expect_true(written_out == n_out, "roundtrip out count");

    // Compare after group-delay settling.  Prototype length 5880 at 1.764 MHz
    // ~ 5880/40 = 147 samples at 44.1 kHz one way; round-trip ~300 samples.
    const uint32_t skip = 400;
    const uint32_t compare_n = std::min(n_in, written_out);
    if (compare_n <= skip + 100) {
        expect_true(false, "roundtrip too short to compare");
        return;
    }

    // Search small lag window for best correlation (integer delay).
    double best_err = 1e9;
    int best_lag = 0;
    for (int lag = 0; lag < 400; lag++) {
        double acc = 0.0;
        uint32_t count = 0;
        for (uint32_t i = skip; i + (uint32_t)lag < compare_n && i < n_in; i++) {
            double d = (double)recovered[i + (uint32_t)lag] - (double)original[i];
            acc += d * d;
            count++;
        }
        if (count > 0) {
            double mse = acc / (double)count;
            if (mse < best_err) {
                best_err = mse;
                best_lag = lag;
            }
        }
    }
    const double rmse = std::sqrt(best_err);
    printf("Round-trip 1 kHz tone: best_lag=%d rmse=%.6f\n", best_lag, rmse);
    expect_true(rmse < 0.05, "round-trip RMSE under 0.05 for 1 kHz tone");
}

int main() {
    printf("=== AudioCodecFilter_44100_12000_PolyphaseFIR smoke tests ===\n");
    test_rate_ratio();
    test_expand_rate();
    test_roundtrip_tone();

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("All filter 44.1↔12 tests passed.\n");
    return 0;
}
