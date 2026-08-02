// HD codec test harness — sibling to tests.cpp
//
// Exercises the 48 kHz -> 24 kHz HD path:
//   AudioCodecFilter_2X_PolyphaseFIR + AudioCodecCompressor_12BitVbrDelta
//
// Uses speech WAVs from Tests/TestData/SpeechFiles (same set as the default harness).
// Classic THD is defined for pure sinusoids; speech residuals are not. This harness
// still reports a clearly labelled residual harmonic-distortion proxy on residual =
// aligned_input - decoded, plus SNR / RMS metrics. See analyseResidualDistortion().
//
// Also reports a rough band-edge / aliasing spill proxy: residual spectral energy
// in a guard band near the 24 kHz-path Nyquist (~12 kHz at the 48 kHz I/O rate),
// normalised against a broader midband reference. Not a standards metric.
//
// Before the speech encode/decode loop, measures the 2X FIR cascade (decimate+expand
// only) with a coarse sine sweep so the harness prints approximate −3 dB edge and
// stopband behaviour for the speech-first redesign (~8.5 kHz passband / ≥70 dB by
// 12 kHz).

#include "../Codecs/AudioCodec.hpp"
#include "../Codecs/Filters/AudioCodecFilter_2X_PolyphaseFIR.hpp"
#include "../Codecs/Filters/AnogHDPostDecodeLpf.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_12BitVbrDelta.hpp"
#include "../Codecs/Squelchers/AudioCodecSquelcher_Basic.hpp"
#include "../Codecs/Utility/AudioFileUtils.hpp"
#include "AudioFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// MSVC does not define M_PI in <cmath> unless _USE_MATH_DEFINES is set.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SampleType AudioS16

#define SampleRate      48000
#define NumInputSamples 1024

static const std::string testFileFolder = "../Tests/TestData/SpeechFiles/";
static const std::string testFile1 = testFileFolder + "0016_000013.wav";
static const std::string testFile2 = testFileFolder + "0016_000022.wav";
static const std::string testFile3 = testFileFolder + "0016_000255.wav";
static const std::string testFile4 = testFileFolder + "0016_000350.wav";

// ---------------------------------------------------------------------------
// Small analysis helpers (test-only; do not alter core codec logic)
// ---------------------------------------------------------------------------

static double sampleToDouble(SampleType s) {
    return static_cast<double>(s);
}

static double computeRms(const double *x, int n) {
    if (n <= 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += x[i] * x[i];
    return std::sqrt(acc / static_cast<double>(n));
}

// Best integer delay of `decoded` vs `input` over [0, maxDelay], minimising residual energy.
static int findBestDelay(const SampleType *input, const SampleType *decoded, int n, int maxDelay) {
    int bestDelay = 0;
    double bestEnergy = 1e300;
    const int searchMax = std::min(maxDelay, n - 2);
    for (int d = 0; d <= searchMax; ++d) {
        double energy = 0.0;
        const int count = n - d;
        for (int i = 0; i < count; ++i) {
            const double r = sampleToDouble(input[i]) - sampleToDouble(decoded[i + d]);
            energy += r * r;
        }
        energy /= static_cast<double>(count);
        if (energy < bestEnergy) {
            bestEnergy = energy;
            bestDelay = d;
        }
    }
    return bestDelay;
}

// Autocorrelation lag of peak in [minLag, maxLag] — rough F0 proxy for a window.
static int estimatePitchLag(const double *x, int n, int minLag, int maxLag) {
    maxLag = std::min(maxLag, n - 1);
    minLag = std::max(minLag, 1);
    if (maxLag < minLag) return 0;

    double bestCorr = -1e300;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double corr = 0.0;
        for (int i = 0; i < n - lag; ++i) corr += x[i] * x[i + lag];
        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag = lag;
        }
    }
    return bestLag;
}

// Magnitude of a single DFT bin (Goertzel) at frequency binFreqHz.
static double goertzelMagnitude(const double *x, int n, double sampleRate, double binFreqHz) {
    if (n <= 0 || binFreqHz <= 0.0 || binFreqHz >= sampleRate * 0.5) return 0.0;
    const double omega = 2.0 * M_PI * binFreqHz / sampleRate;
    const double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos(omega);
    const double imag = s2 * std::sin(omega);
    return std::sqrt(real * real + imag * imag);
}

// Rough residual spectral energy in [loHz, hiHz] via summed Goertzel |X|^2
// at DFT-bin centres (df = sampleRate / n). Pragmatic periodogram proxy only.
static double bandEnergyGoertzel(const double *x, int n, double sampleRate,
                                 double loHz, double hiHz) {
    if (n <= 0 || hiHz <= loHz) return 0.0;
    const double nyquist = sampleRate * 0.5;
    loHz = std::max(loHz, 0.0);
    hiHz = std::min(hiHz, nyquist - 1.0);
    if (hiHz <= loHz) return 0.0;

    const double df = sampleRate / static_cast<double>(n);
    double energy = 0.0;
    // Start at the first bin centre at or above loHz.
    const int k0 = static_cast<int>(std::ceil(loHz / df));
    const int k1 = static_cast<int>(std::floor(hiHz / df));
    for (int k = k0; k <= k1; ++k) {
        const double f = static_cast<double>(k) * df;
        if (f <= 0.0 || f >= nyquist) continue;
        const double mag = goertzelMagnitude(x, n, sampleRate, f);
        energy += mag * mag;
    }
    return energy;
}

struct ResidualMetrics {
    double inputRms = 0.0;
    double residualRms = 0.0;
    double snrDb = 0.0;
    // Residual harmonic-distortion proxy (NOT classic THD of a sinusoid).
    // For windows where residual energy is usable: estimate residual F0 via
    // autocorrelation, measure Goertzel magnitude at k*F0 (k=1..harmonics),
    // then report 20*log10(sqrt(sum_{k=2..N} |H_k|^2) / |H_1|).
    // Averaged over analysis windows that pass the energy gate.
    double residualThdProxyDb = 0.0;
    // Rough aliasing / band-edge spill proxy (NOT a standards metric).
    // 10*log10(E_guard / E_ref) on residual, where guard ≈ 10–12 kHz (near
    // internal 24 kHz Nyquist) and ref is a broader midband below that edge.
    // Higher (less negative) => more residual energy piled near the band edge.
    double bandEdgeSpillProxyDb = 0.0;
    int windowsAnalysed = 0;
    int spillWindowsAnalysed = 0;
    int delaySamples = 0;
};

// Analyse residual = aligned_input - decoded.
//
// NOTE ON "RESIDUAL THD":
// Classic THD assumes a pure-tone stimulus and measures harmonic products relative
// to that tone's fundamental. Speech is broadband and non-stationary, so classic
// THD is not well-defined here. What we report is a best-effort residual harmonic
// distortion *proxy* on the error signal itself: how tonally harmonic the residual
// looks around its own dominant period. Treat it as a relative codec-quality
// indicator across runs, not as a standards-compliant THD figure.
//
// NOTE ON "BAND-EDGE SPILL":
// The HD path decimates 48->24 kHz, so the critical band edge / first-alias region
// sits near 12 kHz at the 48 kHz I/O rate. We estimate residual energy in a
// configurable guard band just below that edge and normalise it against a broader
// midband reference on the same residual window. This is a rough relative proxy
// for spectral pile-up near Nyquist (possible aliasing / filter-edge leakage),
// not a standards aliasing or stopband measurement.
static ResidualMetrics analyseResidualDistortion(const SampleType *input,
                                                 const SampleType *decoded,
                                                 int numSamples,
                                                 int sampleRate) {
    ResidualMetrics m;
    // 2X polyphase FIR (128-tap) + expand cascade ≈ ~127 samples group delay at 48 kHz.
    // Search past that so residual metrics are not dominated by alignment error.
    const int maxDelay = 192;
    m.delaySamples = findBestDelay(input, decoded, numSamples, maxDelay);

    const int n = numSamples - m.delaySamples;
    if (n <= 0) return m;

    std::vector<double> residual(static_cast<size_t>(n));
    std::vector<double> alignedIn(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        alignedIn[static_cast<size_t>(i)] = sampleToDouble(input[i]);
        residual[static_cast<size_t>(i)] =
            sampleToDouble(input[i]) - sampleToDouble(decoded[i + m.delaySamples]);
    }

    m.inputRms = computeRms(alignedIn.data(), n);
    m.residualRms = computeRms(residual.data(), n);
    if (m.residualRms > 1e-12 && m.inputRms > 1e-12) {
        m.snrDb = 20.0 * std::log10(m.inputRms / m.residualRms);
    } else if (m.residualRms <= 1e-12) {
        m.snrDb = 200.0; // effectively clean
    }

    // Configurable guard / reference bands for the band-edge spill proxy.
    // Guard: near internal Nyquist (~12 kHz). Ref: broader midband below the edge.
    const double guardLoHz = 10000.0;
    const double guardHiHz = 12000.0;
    const double refLoHz   = 500.0;
    const double refHiHz   = 9000.0;

    // Windowed residual harmonic proxy + band-edge spill proxy.
    // Prefer 1024-sample windows; fall back to 512 when the aligned buffer is short
    // (e.g. single-packet analysis after ~127-sample FIR delay compensation).
    int windowSize = 1024;
    if (n < windowSize) windowSize = 512;
    if (n < windowSize) {
        // Too short for spectral proxies; SNR/RMS still valid.
        return m;
    }
    const int hop = windowSize / 2;
    const int harmonics = 5; // fundamental + 2..5
    // Speech F0 ~ 80–400 Hz at 48 kHz => lags ~120–600 samples.
    const int minLag = sampleRate / 400;
    const int maxLag = sampleRate / 80;

    double thdProxyLinAcc = 0.0;
    int thdCount = 0;
    double spillRatioLinAcc = 0.0;
    int spillCount = 0;

    for (int start = 0; start + windowSize <= n; start += hop) {
        const double *win = residual.data() + start;
        const double winRms = computeRms(win, windowSize);
        // Skip near-silent residual windows (nothing meaningful to analyse).
        if (winRms < (m.inputRms * 1e-4) + 1e-6) continue;

        // Band-edge spill: does not need a pitch estimate; run on every gated window.
        {
            const double eGuard =
                bandEnergyGoertzel(win, windowSize, sampleRate, guardLoHz, guardHiHz);
            const double eRef =
                bandEnergyGoertzel(win, windowSize, sampleRate, refLoHz, refHiHz);
            if (eRef > 1e-18) {
                spillRatioLinAcc += eGuard / eRef;
                ++spillCount;
            }
        }

        const int lag = estimatePitchLag(win, windowSize, minLag, maxLag);
        if (lag <= 0) continue;
        const double f0 = static_cast<double>(sampleRate) / static_cast<double>(lag);
        if (f0 * harmonics >= sampleRate * 0.5) continue;

        const double fund = goertzelMagnitude(win, windowSize, sampleRate, f0);
        if (fund < 1e-9) continue;

        double harmPower = 0.0;
        for (int k = 2; k <= harmonics; ++k) {
            const double hk = goertzelMagnitude(win, windowSize, sampleRate, f0 * k);
            harmPower += hk * hk;
        }
        const double ratio = std::sqrt(harmPower) / fund;
        thdProxyLinAcc += ratio;
        ++thdCount;
    }

    m.windowsAnalysed = thdCount;
    if (thdCount > 0) {
        const double meanRatio = thdProxyLinAcc / static_cast<double>(thdCount);
        m.residualThdProxyDb = 20.0 * std::log10(std::max(meanRatio, 1e-12));
    } else {
        m.residualThdProxyDb = -200.0; // no usable windows
    }

    m.spillWindowsAnalysed = spillCount;
    if (spillCount > 0) {
        const double meanSpill = spillRatioLinAcc / static_cast<double>(spillCount);
        m.bandEdgeSpillProxyDb = 10.0 * std::log10(std::max(meanSpill, 1e-18));
    } else {
        m.bandEdgeSpillProxyDb = -200.0; // no usable windows
    }
    return m;
}

// ---------------------------------------------------------------------------
// 2X FIR cascade frequency-response probe (filter only; no compressor)
// ---------------------------------------------------------------------------
//
// Drive decimate+expand with steady sines, compare settled output RMS to input
// RMS after discarding FIR group delay. Coarse but enough to state approximate
// −3 dB edge and stopband depth at the internal Nyquist.

struct FilterSweepPoint {
    double freqHz = 0.0;
    double gainDb = 0.0;
};

struct FilterResponseSummary {
    double f3dBHz = 0.0;
    double gainAt8500Db = 0.0;
    double gainAt12000Db = 0.0;
    bool found3dB = false;
    std::vector<FilterSweepPoint> points;
};

// Use float samples for the probe so deep stopband attenuation is not
// crushed to the int16 quantisation floor (speech path still uses SampleType).
static double measureCascadeGainDb(AudioCodecFilter_2X_PolyphaseFIR<AudioF32> &filter,
                                   double freqHz,
                                   int sampleRate) {
    // Long enough for FIR settle (~127 samples) + several periods at low freqs.
    const int totalSamples = 8192;
    const int skipSamples = 512; // discard transient / group-delay region
    const int measureSamples = totalSamples - skipSamples;
    if (measureSamples <= 0) return -200.0;

    std::vector<AudioF32> input(static_cast<size_t>(totalSamples));
    std::vector<AudioF32> mid(static_cast<size_t>(totalSamples / 2));
    std::vector<AudioF32> output(static_cast<size_t>(totalSamples));

    const float amp = 0.5f;
    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        input[static_cast<size_t>(i)] =
            static_cast<AudioF32>(std::sin(2.0 * M_PI * freqHz * t) * amp);
    }

    filter.reset();
    uint32_t nMid = static_cast<uint32_t>(mid.size());
    filter.decimate(input.data(), static_cast<uint32_t>(totalSamples), mid.data(), &nMid);
    uint32_t nOut = static_cast<uint32_t>(output.size());
    filter.expand(mid.data(), nMid, output.data(), &nOut);

    // Align roughly by FIR cascade delay (~127 samples at 48 kHz) then refine
    // with a short correlation search so RMS isn't killed by phase.
    const int nomDelay = 127;
    const int searchPad = 16;
    int bestDelay = nomDelay;
    double bestCorr = -1e300;
    const int d0 = std::max(0, nomDelay - searchPad);
    const int d1 = std::min(skipSamples, nomDelay + searchPad);
    const int corrLen = 256;
    for (int d = d0; d <= d1; ++d) {
        double corr = 0.0;
        for (int i = 0; i < corrLen; ++i) {
            corr += static_cast<double>(input[static_cast<size_t>(skipSamples + i)]) *
                    static_cast<double>(output[static_cast<size_t>(skipSamples + i + d)]);
        }
        if (corr > bestCorr) {
            bestCorr = corr;
            bestDelay = d;
        }
    }

    double inAcc = 0.0;
    double outAcc = 0.0;
    int count = 0;
    for (int i = skipSamples; i + bestDelay < static_cast<int>(nOut) && i < totalSamples; ++i) {
        const double xin = static_cast<double>(input[static_cast<size_t>(i)]);
        const double xout = static_cast<double>(output[static_cast<size_t>(i + bestDelay)]);
        inAcc += xin * xin;
        outAcc += xout * xout;
        ++count;
    }
    if (count <= 0 || inAcc <= 0.0) return -200.0;
    const double inRms = std::sqrt(inAcc / static_cast<double>(count));
    const double outRms = std::sqrt(outAcc / static_cast<double>(count));
    if (inRms <= 1e-18) return -200.0;
    const double ratio = outRms / inRms;
    return 20.0 * std::log10(std::max(ratio, 1e-18));
}

static FilterResponseSummary measureFilterCascadeResponse(int sampleRate) {
    FilterResponseSummary summary;
    AudioCodecFilter_2X_PolyphaseFIR<AudioF32> filter;

    // Coarse sweep focused on passband / transition / Nyquist.
    static const double kSweepHz[] = {
        1000.0, 4000.0, 6000.0, 7500.0, 8500.0, 9000.0,
        9500.0, 9800.0, 10000.0, 10500.0, 11000.0, 12000.0, 14000.0
    };
    const int nPts = static_cast<int>(sizeof(kSweepHz) / sizeof(kSweepHz[0]));
    summary.points.reserve(static_cast<size_t>(nPts));

    double prevFreq = 0.0;
    double prevGain = 0.0;
    bool havePrev = false;

    for (int i = 0; i < nPts; ++i) {
        FilterSweepPoint pt;
        pt.freqHz = kSweepHz[i];
        pt.gainDb = measureCascadeGainDb(filter, pt.freqHz, sampleRate);
        summary.points.push_back(pt);

        if (std::abs(pt.freqHz - 8500.0) < 0.5) summary.gainAt8500Db = pt.gainDb;
        if (std::abs(pt.freqHz - 12000.0) < 0.5) summary.gainAt12000Db = pt.gainDb;

        if (!summary.found3dB && havePrev && prevGain > -3.0 && pt.gainDb <= -3.0) {
            // Linear interpolation in dB vs frequency between the straddling points.
            const double t = (-3.0 - prevGain) / (pt.gainDb - prevGain);
            summary.f3dBHz = prevFreq + t * (pt.freqHz - prevFreq);
            summary.found3dB = true;
        }
        prevFreq = pt.freqHz;
        prevGain = pt.gainDb;
        havePrev = true;
    }
    return summary;
}

// ---------------------------------------------------------------------------
// Regression: full-scale polarity flip must not desync 12-bit VBR
// ---------------------------------------------------------------------------
//
// Pre-fix: sample steps of ±2047 produced |delta| up to 4094. Packing only 12
// bits made +2048 collide with the end-of-run marker → bitstream desync.
// Fix: wrap deltas/accumulator in signed 12-bit (mirror 8-bit ANOG int8 wrap);
// only -2048 is nudged with a 1-LSB propagate. Polarity flips round-trip cleanly.

static int testFullScalePolarityFlipRoundtrip() {
    printf("--- Regression: full-scale polarity-flip 12-bit VBR round-trip ---\n");

    constexpr uint32_t N = 256;
    AudioCodecCompressor_12BitVbrDelta<SampleType> compressor;
    std::vector<SampleType> input(N);
    std::vector<SampleType> output(N, 0);

    // Full-scale step mid-frame → after 12BitScaled, a ±2047 polarity flip.
    for (uint32_t i = 0; i < N; ++i) {
        input[i] = (i < N / 2)
            ? static_cast<SampleType>(-32767)
            : static_cast<SampleType>(32767);
    }

    const uint32_t maxBytes = compressor.getBytesMaxCompressedSize(N);
    std::vector<uint8_t> compressed(maxBytes, 0);
    uint32_t written = maxBytes;
    compressor.compress(input.data(), N, compressed.data(), &written);

    uint32_t outN = N;
    compressor.decompress(compressed.data(), written, output.data(), &outN);

    if (outN != N) {
        printf("FAIL: decoded %u samples, expected %u (likely bitstream desync)\n", outN, N);
        return 1;
    }

    // With 12-bit wrap, the step reconstructs; allow only 12-bit quantisation error.
    constexpr int32_t kMaxAbsErr = 512;
    int bad = 0;
    int32_t maxAbsErr = 0;
    for (uint32_t i = 0; i < N; ++i) {
        const int32_t err = std::abs(static_cast<int32_t>(input[i]) -
                                     static_cast<int32_t>(output[i]));
        if (err > maxAbsErr) maxAbsErr = err;
        if (err > kMaxAbsErr) ++bad;
    }

    if (bad > 0) {
        printf("FAIL: %d samples exceed abs err %d (maxAbsErr=%d)\n",
               bad, kMaxAbsErr, maxAbsErr);
        return 1;
    }

    printf("PASS: polarity-flip round-trip (written=%u bytes, maxAbsErr=%d)\n\n",
           written, maxAbsErr);
    return 0;
}

// ---------------------------------------------------------------------------
// Regression: getBytesMaxCompressedSize must cover multi-run EOR overhead
// ---------------------------------------------------------------------------
//
// Old formula assumed one 12-bit run (one length code, no EORs). A mid-frame
// silence island forces run transitions; encode can exceed that budget and
// overrun the caller buffer (Amy: panic [:771] with capacity 770).

static int testMaxCompressedSizeCoversEORRuns() {
    printf("--- Regression: max compressed size covers EOR run transitions ---\n");

    constexpr uint32_t N = 512; // voice packet after 2× decimation
    AudioCodecCompressor_12BitVbrDelta<SampleType> compressor;
    std::vector<SampleType> input(N);

    // Loud → short silence → loud: forces at least one EOR between 12-bit runs.
    for (uint32_t i = 0; i < N; ++i) {
        if (i >= N / 3 && i < N / 3 + 16) {
            input[i] = 0;
        } else {
            input[i] = (i & 1)
                ? static_cast<SampleType>(-32767)
                : static_cast<SampleType>(32767);
        }
    }

    const uint32_t maxBytes = compressor.getBytesMaxCompressedSize(N);
    // Pre-fix single-run budget for N=512 was 770.
    constexpr uint32_t kOldSingleRunBudget = 770;
    if (maxBytes <= kOldSingleRunBudget) {
        printf("FAIL: maxBytes=%u still looks like the old single-run formula\n", maxBytes);
        return 1;
    }

    std::vector<uint8_t> compressed(maxBytes, 0);
    uint32_t written = maxBytes;
    compressor.compress(input.data(), N, compressed.data(), &written);

    if (written > maxBytes) {
        printf("FAIL: wrote %u bytes into buffer of %u\n", written, maxBytes);
        return 1;
    }
    if (written <= kOldSingleRunBudget) {
        // Soft check: this stimulus should usually exceed the old budget; if not,
        // still OK as long as maxBytes is correct — but flag for visibility.
        printf("NOTE: written=%u did not exceed old budget %u (stimulus may have merged)\n",
               written, kOldSingleRunBudget);
    }

    uint32_t outN = N;
    std::vector<SampleType> output(N, 0);
    compressor.decompress(compressed.data(), written, output.data(), &outN);
    if (outN != N) {
        printf("FAIL: decoded %u samples, expected %u\n", outN, N);
        return 1;
    }

    printf("PASS: maxBytes=%u written=%u (old single-run budget was %u)\n\n",
           maxBytes, written, kOldSingleRunBudget);
    return 0;
}

// ---------------------------------------------------------------------------
// Regression: post-decode Butterworth LPF meets 10 kHz / −24 dB @ 12 kHz
// ---------------------------------------------------------------------------

static int testPostDecodeLpfResponse() {
    printf("--- Regression: AnogHD post-decode LPF response ---\n");
    const float g1 = AnogHDPostDecodeLpf::measureGainDb(1000.f);
    const float g10 = AnogHDPostDecodeLpf::measureGainDb(10000.f);
    const float g12 = AnogHDPostDecodeLpf::measureGainDb(12000.f);
    printf("Gain @ 1 kHz: %+.2f dB (expect ~0)\n", g1);
    printf("Gain @ 10 kHz: %+.2f dB (expect ~-3)\n", g10);
    printf("Gain @ 12 kHz: %+.2f dB (expect <= -24)\n", g12);

    if (g1 > 1.f || g1 < -1.f) {
        printf("FAIL: passband not flat at 1 kHz\n");
        return 1;
    }
    if (g10 > -2.f || g10 < -5.f) {
        printf("FAIL: expected ~-3 dB at 10 kHz cutoff\n");
        return 1;
    }
    if (g12 > -24.f) {
        printf("FAIL: need <= -24 dB at 12 kHz\n");
        return 1;
    }
    printf("PASS: post-decode LPF response\n\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Main HD encode/decode harness
// ---------------------------------------------------------------------------

int main() {
    printf("=== HD Audio Codec Test Harness ===\n");
    printf("Path: 2X polyphase FIR (48->24 kHz) + 12-bit VBR delta\n");
    printf("Filter design: speech-first (~8.5 kHz passband, >=70 dB by 12 kHz)\n");
    printf("NOTE: Residual THD below is a speech residual harmonic-distortion\n");
    printf("      proxy, not classic pure-tone THD. See comments in tests_hd.cpp.\n");
    printf("NOTE: Band-edge spill is a rough residual spectral proxy near ~12 kHz\n");
    printf("      (internal Nyquist), not a standards aliasing metric.\n\n");

    if (testFullScalePolarityFlipRoundtrip() != 0) {
        return 1;
    }
    if (testMaxCompressedSizeCoversEORRuns() != 0) {
        return 1;
    }
    if (testPostDecodeLpfResponse() != 0) {
        return 1;
    }

    // Filter-only cascade response (decimate+expand), before speech codec loop.
    {
        printf("--- 2X FIR cascade frequency response (sine-sweep probe) ---\n");
        printf("Stimulus: steady sines through decimate+expand only (no compressor).\n");
        const FilterResponseSummary fr = measureFilterCascadeResponse(SampleRate);
        for (const FilterSweepPoint &pt : fr.points) {
            printf("  %7.0f Hz : %+7.2f dB\n", pt.freqHz, pt.gainDb);
        }
        if (fr.found3dB) {
            printf("Approx cascade -3 dB edge: ~%.0f Hz\n", fr.f3dBHz);
        } else {
            printf("Approx cascade -3 dB edge: (not crossed in sweep grid)\n");
        }
        printf("Gain @ 8500 Hz (passband target): %+.2f dB\n", fr.gainAt8500Db);
        printf("Gain @ 12000 Hz (internal Nyquist): %+.2f dB  (target <= -70 dB)\n",
               fr.gainAt12000Db);
        printf("(Prototype FIR docs: ~9810 Hz -3 dB, ~100 dB @ 12 kHz; cascade is |H|^2.)\n\n");
    }

    // Pick one of the speech files (same pool as the default harness).
    const std::string testFilename = testFile4;

    AudioFile<SampleType> testFile;
    AudioFile<SampleType> outFile;

    std::string compressedFilename = stripFileExtension(testFilename).append("_hd_comp.vbi");
    std::ofstream compressedFile(compressedFilename, std::ios::trunc | std::ios::binary);
    if (!compressedFile.is_open()) {
        std::cerr << "Error: Unable to open output file at " << compressedFilename << std::endl;
        return -1;
    }

    if (!testFile.load(testFilename)) {
        std::cerr << "Error: Unable to load test WAV: " << testFilename << std::endl;
        return -1;
    }

    const int inputBitDepth = testFile.getBitDepth();
    const int inputSampleRate = testFile.getSampleRate();
    const float inputLengthSeconds = testFile.getLengthInSeconds();
    printf("Test File: %s\n", testFilename.c_str());
    printf("Bit Depth: %d, Sample Rate: %d, Length: %0.2f s\n",
           inputBitDepth, inputSampleRate, inputLengthSeconds);

    outFile.setBitDepth(inputBitDepth);
    outFile.setSampleRate(inputSampleRate);

    AudioCodec<SampleType> codec;
    AudioCodecFilter_2X_PolyphaseFIR<SampleType> codecFilter;
    AudioCodecCompressor_12BitVbrDelta<SampleType> codecCompressor;
    AudioCodecSquelcher_Basic<SampleType> codecSquelcher;
    codec.setFilter(codecFilter);
    codec.setCompressor(codecCompressor);
    codec.setSquelcher(codecSquelcher);

    const uint32_t ioBufferSize = NumInputSamples * sizeof(SampleType);
    SampleType *inputBuffer = static_cast<SampleType *>(malloc(ioBufferSize));
    SampleType *outputBuffer = static_cast<SampleType *>(malloc(ioBufferSize));
    const uint32_t compBufferSize = codec.getMaxEncodedBytes(NumInputSamples);
    uint8_t *compBuffer = static_cast<uint8_t *>(malloc(compBufferSize));
    printf("Packet size: %d samples (%.2f ms at %d Hz)\n",
           NumInputSamples,
           1000.0 * NumInputSamples / static_cast<double>(SampleRate),
           SampleRate);
    printf("Decimated packet: %u samples (24 kHz internal)\n",
           codecFilter.getDecimatedSamples(NumInputSamples));
    printf("Max encoded bytes/packet: %u\n\n", compBufferSize);

    const int numSamples = testFile.getNumSamplesPerChannel();
    outFile.setAudioBufferSize(1, numSamples);

    // Accumulate full-file residual analysis buffers.
    std::vector<SampleType> allInput(static_cast<size_t>(numSamples), 0);
    std::vector<SampleType> allDecoded(static_cast<size_t>(numSamples), 0);

    float compRatio12_acc = 0.0f;
    float compBufferWritten_acc = 0.0f;
    int numPackets = 0;
    uint32_t compFileLength = 0;
    int decodeMismatchPackets = 0;

    // VBIF flag is only 8-vs-16; mark HD streams as non-8-bit (16).
    AudioFileUtils::vbif_header compHeader;
    const uint32_t frameLength = codecFilter.getDecimatedSamples(NumInputSamples);
    compHeader = AudioFileUtils::Make_VBIF_Header(16, inputSampleRate, frameLength);
    compressedFile.write(reinterpret_cast<const char *>(&compHeader), sizeof(compHeader));
    compFileLength += sizeof(compHeader);

    if (numSamples <= NumInputSamples) {
        printf("Can't encode this file - too few samples. Must have at least %d samples.\n",
               NumInputSamples);
        free(inputBuffer);
        free(outputBuffer);
        free(compBuffer);
        return -1;
    }

    for (int offset = 0; offset < (numSamples - (NumInputSamples - 1)); offset += NumInputSamples) {
        for (int i = 0; i < NumInputSamples; ++i) {
            inputBuffer[i] = testFile.samples[0][i + offset];
            allInput[static_cast<size_t>(i + offset)] = inputBuffer[i];
        }

        printf("--- HD packet %d @ offset %d ---\n", numPackets, offset);

        uint32_t outCompBufferWritten = compBufferSize;
        codec.encode(inputBuffer, NumInputSamples, compBuffer, &outCompBufferWritten);
        printf("  Encoded: %u bytes\n", outCompBufferWritten);

        compressedFile.write(reinterpret_cast<const char *>(compBuffer), outCompBufferWritten);
        compFileLength += outCompBufferWritten;

        uint32_t outOutputSamplesWritten = NumInputSamples;
        codec.decode(compBuffer, outCompBufferWritten, outputBuffer, &outOutputSamplesWritten);
        if (outOutputSamplesWritten != NumInputSamples) {
            printf("  Error: decoded %u samples, expected %d\n",
                   outOutputSamplesWritten, NumInputSamples);
            ++decodeMismatchPackets;
        }

        for (int i = 0; i < NumInputSamples; ++i) {
            outFile.samples[0][i + offset] = outputBuffer[i];
            allDecoded[static_cast<size_t>(i + offset)] = outputBuffer[i];
        }

        // Per-packet residual snapshot (uses current packet only).
        ResidualMetrics pktMetrics =
            analyseResidualDistortion(inputBuffer, outputBuffer, NumInputSamples, SampleRate);
        printf("  Packet residual: SNR=%0.2f dB, residual RMS=%0.2f, "
               "residual-THD-proxy=%0.2f dB, "
               "band-edge-spill-proxy=%0.2f dB "
               "(thd-windows=%d, spill-windows=%d, delay=%d)\n",
               pktMetrics.snrDb, pktMetrics.residualRms, pktMetrics.residualThdProxyDb,
               pktMetrics.bandEdgeSpillProxyDb,
               pktMetrics.windowsAnalysed, pktMetrics.spillWindowsAnalysed,
               pktMetrics.delaySamples);

        // Compression vs 12-bit packed PCM of the decimated frame (+ scale byte).
        // 512 samples * 1.5 bytes ≈ packed 12-bit; header is 1 scale + 2 first-sample bytes.
        const float ref12BitBytes =
            static_cast<float>(frameLength) * 1.5f + 3.0f;
        const float compRatio12 = static_cast<float>(outCompBufferWritten) / ref12BitBytes;
        printf("  Compression vs ~12-bit packed ref: %0.2f:1 (%.1f %% of ref)\n",
               1.0f / compRatio12, compRatio12 * 100.0f);

        // Also vs 16-bit PCM at full rate (what the WAV stores).
        const float compRatio16 =
            static_cast<float>(outCompBufferWritten) /
            static_cast<float>(NumInputSamples * sizeof(SampleType));
        printf("  Compression vs 16-bit PCM @48k: %0.2f:1 (%.1f %% of PCM)\n",
               1.0f / compRatio16, compRatio16 * 100.0f);

        compRatio12_acc += compRatio12;
        compBufferWritten_acc += static_cast<float>(outCompBufferWritten);
        ++numPackets;
    }

    printf("\n=== HD summary over %d packets ===\n", numPackets);
    if (numPackets > 0) {
        const float compRatio12_avg = compRatio12_acc / static_cast<float>(numPackets);
        printf("Average compression vs ~12-bit packed ref: %0.2f:1 (%.1f %% of ref)\n",
               1.0f / compRatio12_avg, compRatio12_avg * 100.0f);

        const float avgCompBufferWritten =
            compBufferWritten_acc / static_cast<float>(numPackets);
        const float dataBitRate =
            avgCompBufferWritten / (static_cast<float>(NumInputSamples) / static_cast<float>(SampleRate));
        printf("Average compressed data rate: %0.1f kB/s or %0.0f kbps\n",
               dataBitRate / 1024.0f, dataBitRate / 128.0f);
    }
    printf("Decode sample-count mismatches: %d\n", decodeMismatchPackets);

    // File-level residual analysis on the processed span.
    const int processedSamples = numPackets * NumInputSamples;
    ResidualMetrics fileMetrics = analyseResidualDistortion(
        allInput.data(), allDecoded.data(), processedSamples, SampleRate);
    printf("\n--- File-level residual distortion analysis ---\n");
    printf("Alignment delay used: %d samples\n", fileMetrics.delaySamples);
    printf("Input RMS:            %0.2f\n", fileMetrics.inputRms);
    printf("Residual RMS:         %0.2f\n", fileMetrics.residualRms);
    printf("SNR (input/residual): %0.2f dB\n", fileMetrics.snrDb);
    printf("Residual THD proxy:   %0.2f dB  (mean over %d gated windows)\n",
           fileMetrics.residualThdProxyDb, fileMetrics.windowsAnalysed);
    printf("(Residual THD proxy = 20*log10(sqrt(sum |H2..H5|^2) / |H1|) on residual;\n");
    printf(" classic pure-tone THD is NOT well-defined for speech — treat as relative.)\n");
    printf("Band-edge spill proxy: %0.2f dB  (mean over %d gated windows)\n",
           fileMetrics.bandEdgeSpillProxyDb, fileMetrics.spillWindowsAnalysed);
    printf("(Rough proxy = 10*log10(E[10-12 kHz] / E[0.5-9 kHz]) on residual near\n");
    printf(" internal ~12 kHz Nyquist — NOT a standards aliasing / stopband figure.)\n");

    compressedFile.close();
    printf("\nCompressed file: %s (%u bytes, %0.1f kB)\n",
           compressedFilename.c_str(), compFileLength, compFileLength / 1024.0f);

    const std::string outputFilename = stripFileExtension(testFilename).append("_hd_proc.wav");
    outFile.save(outputFilename);
    printf("Decoded WAV:     %s (%d bytes, %0.1f kB)\n",
           outputFilename.c_str(), outFile.savedLength, outFile.savedLength / 1024.0f);

    free(inputBuffer);
    free(outputBuffer);
    free(compBuffer);

    printf("\nHD harness done.\n\n");
    return (decodeMismatchPackets == 0) ? 0 : 1;
}
