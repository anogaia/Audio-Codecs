// Listening A/B comparison harness — sibling to tests.cpp / tests_hd.cpp
//
// Encodes and decodes the same speech WAV through:
//   Legacy: 4X polyphase FIR (48->12 kHz) + 8-bit VBR delta
//   HD:     2X polyphase FIR (48->24 kHz) + 12-bit VBR delta
//
// Emits decoded WAVs at the input sample rate / bit depth for side-by-side
// audition, plus optional residual WAVs and a short text summary.
// Does not replace or alter the default / HD harnesses.

#include "../Codecs/AudioCodec.hpp"
#include "../Codecs/Filters/AudioCodecFilter_4X_PolyphaseFIR.hpp"
#include "../Codecs/Filters/AudioCodecFilter_2X_PolyphaseFIR.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_8BitVbrDelta.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_12BitVbrDelta.hpp"
#include "../Codecs/Squelchers/AudioCodecSquelcher_Basic.hpp"
#include "../Codecs/Utility/AudioFileUtils.hpp"
#include "AudioFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define SampleType AudioS16

#define SampleRate      48000
#define NumInputSamples 1024

static const std::string testFileFolder = "../Tests/TestData/SpeechFiles/";
static const std::string outFolder      = "../Tests/TestData/ListeningCompare/";

// Same speech pool as the other harnesses; pick one short-ish clip for quick A/B.
static const std::string testFile1 = testFileFolder + "0016_000013.wav";
static const std::string testFile2 = testFileFolder + "0016_000022.wav";
static const std::string testFile3 = testFileFolder + "0016_000255.wav";
static const std::string testFile4 = testFileFolder + "0016_000350.wav";

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

struct PathMetrics {
    double inputRms = 0.0;
    double residualRms = 0.0;
    double snrDb = 0.0;
    int delaySamples = 0;
    int processedSamples = 0;
    uint32_t compressedBytes = 0;
    int decodeMismatchPackets = 0;
    int numPackets = 0;
};

// Build aligned residual and SNR for a processed span. Also fills residualOut
// (same length as processedSamples) with delay-compensated error, zero-padded
// at the end where alignment eats samples.
static PathMetrics analyseAndBuildResidual(const SampleType *input,
                                           const SampleType *decoded,
                                           int numSamples,
                                           int maxDelay,
                                           std::vector<SampleType> *residualOut) {
    PathMetrics m;
    m.processedSamples = numSamples;
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
        m.snrDb = 200.0;
    }

    if (residualOut) {
        residualOut->assign(static_cast<size_t>(numSamples), 0);
        for (int i = 0; i < n; ++i) {
            double r = residual[static_cast<size_t>(i)];
            // Soft clip to int16 range for auditionable residual WAV.
            if (r > 32767.0) r = 32767.0;
            if (r < -32768.0) r = -32768.0;
            (*residualOut)[static_cast<size_t>(i)] = static_cast<SampleType>(r);
        }
    }
    return m;
}

static std::string fileStem(const std::string &path) {
    return std::filesystem::path(path).stem().string();
}

static bool saveMonoWav(const std::string &path,
                        const SampleType *samples,
                        int numSamples,
                        int bitDepth,
                        int sampleRate) {
    AudioFile<SampleType> out;
    out.setBitDepth(bitDepth);
    out.setSampleRate(sampleRate);
    out.setAudioBufferSize(1, numSamples);
    for (int i = 0; i < numSamples; ++i) {
        out.samples[0][static_cast<size_t>(i)] = samples[i];
    }
    if (!out.save(path)) {
        std::cerr << "Error: failed to save WAV: " << path << std::endl;
        return false;
    }
    printf("  Wrote %s (%d bytes, %.1f kB)\n",
           path.c_str(), out.savedLength, out.savedLength / 1024.0f);
    return true;
}

// Run one codec path over the full file (packetised), filling decodedOut / metrics.
template <typename FilterT, typename CompressorT>
static PathMetrics runPath(const char *label,
                           FilterT &filter,
                           CompressorT &compressor,
                           const AudioFile<SampleType> &testFile,
                           uint8_t vbifBitWidthFlag,
                           std::vector<SampleType> &decodedOut,
                           const std::string &compressedPath) {
    PathMetrics metrics;
    AudioCodec<SampleType> codec;
    AudioCodecSquelcher_Basic<SampleType> squelcher;
    codec.setFilter(filter);
    codec.setCompressor(compressor);
    codec.setSquelcher(squelcher);

    const int numSamples = testFile.getNumSamplesPerChannel();
    const int inputSampleRate = testFile.getSampleRate();
    decodedOut.assign(static_cast<size_t>(numSamples), 0);

    SampleType *inputBuffer = static_cast<SampleType *>(malloc(NumInputSamples * sizeof(SampleType)));
    SampleType *outputBuffer = static_cast<SampleType *>(malloc(NumInputSamples * sizeof(SampleType)));
    const uint32_t compBufferSize = codec.getMaxEncodedBytes(NumInputSamples);
    uint8_t *compBuffer = static_cast<uint8_t *>(malloc(compBufferSize));

    std::ofstream compressedFile(compressedPath, std::ios::trunc | std::ios::binary);
    if (!compressedFile.is_open()) {
        std::cerr << "Error: Unable to open compressed output: " << compressedPath << std::endl;
        free(inputBuffer);
        free(outputBuffer);
        free(compBuffer);
        metrics.decodeMismatchPackets = -1;
        return metrics;
    }

    const uint32_t frameLength = filter.getDecimatedSamples(NumInputSamples);
    AudioFileUtils::vbif_header compHeader =
        AudioFileUtils::Make_VBIF_Header(vbifBitWidthFlag, inputSampleRate, frameLength);
    compressedFile.write(reinterpret_cast<const char *>(&compHeader), sizeof(compHeader));
    metrics.compressedBytes += sizeof(compHeader);

    printf("\n--- %s ---\n", label);
    printf("  Decimated packet: %u samples\n", frameLength);
    printf("  Max encoded bytes/packet: %u\n", compBufferSize);

    for (int offset = 0; offset < (numSamples - (NumInputSamples - 1)); offset += NumInputSamples) {
        for (int i = 0; i < NumInputSamples; ++i) {
            inputBuffer[i] = testFile.samples[0][static_cast<size_t>(i + offset)];
        }

        uint32_t outCompBufferWritten = compBufferSize;
        codec.encode(inputBuffer, NumInputSamples, compBuffer, &outCompBufferWritten);
        compressedFile.write(reinterpret_cast<const char *>(compBuffer), outCompBufferWritten);
        metrics.compressedBytes += outCompBufferWritten;

        uint32_t outOutputSamplesWritten = NumInputSamples;
        codec.decode(compBuffer, outCompBufferWritten, outputBuffer, &outOutputSamplesWritten);
        if (outOutputSamplesWritten != NumInputSamples) {
            ++metrics.decodeMismatchPackets;
        }

        for (int i = 0; i < NumInputSamples; ++i) {
            decodedOut[static_cast<size_t>(i + offset)] = outputBuffer[i];
        }

        ++metrics.numPackets;
    }

    compressedFile.close();
    metrics.processedSamples = metrics.numPackets * NumInputSamples;

    const float seconds =
        static_cast<float>(metrics.processedSamples) / static_cast<float>(SampleRate);
    const float kbps =
        (seconds > 0.0f)
            ? (static_cast<float>(metrics.compressedBytes) * 8.0f / seconds) / 1000.0f
            : 0.0f;
    printf("  Packets: %d, compressed: %u bytes (%.1f kB), ~%.0f kbps\n",
           metrics.numPackets, metrics.compressedBytes,
           metrics.compressedBytes / 1024.0f, kbps);
    printf("  Compressed file: %s\n", compressedPath.c_str());

    free(inputBuffer);
    free(outputBuffer);
    free(compBuffer);
    return metrics;
}

int main() {
    printf("=== Listening A/B Comparison Harness ===\n");
    printf("Legacy: 4X FIR (48->12 kHz) + 8-bit VBR delta\n");
    printf("HD:     2X FIR (48->24 kHz) + 12-bit VBR delta\n");
    printf("Goal: same-rate/bit-depth decoded WAVs for side-by-side audition.\n\n");

    const std::string testFilename = testFile4;
    const std::string stem = fileStem(testFilename);

    std::error_code ec;
    std::filesystem::create_directories(outFolder, ec);
    if (ec) {
        std::cerr << "Error: Unable to create output folder " << outFolder
                  << ": " << ec.message() << std::endl;
        return -1;
    }

    AudioFile<SampleType> testFile;
    if (!testFile.load(testFilename)) {
        std::cerr << "Error: Unable to load test WAV: " << testFilename << std::endl;
        return -1;
    }

    const int inputBitDepth = testFile.getBitDepth();
    const int inputSampleRate = testFile.getSampleRate();
    const float inputLengthSeconds = testFile.getLengthInSeconds();
    const int numSamples = testFile.getNumSamplesPerChannel();

    printf("Source: %s\n", testFilename.c_str());
    printf("Bit Depth: %d, Sample Rate: %d, Length: %.2f s, Samples: %d\n",
           inputBitDepth, inputSampleRate, inputLengthSeconds, numSamples);
    printf("Output folder: %s\n", outFolder.c_str());

    if (numSamples <= NumInputSamples) {
        printf("Can't encode this file - too few samples. Need at least %d.\n", NumInputSamples);
        return -1;
    }

    // Snapshot input for residual analysis (only the processed span matters).
    std::vector<SampleType> allInput(static_cast<size_t>(numSamples), 0);
    for (int i = 0; i < numSamples; ++i) {
        allInput[static_cast<size_t>(i)] = testFile.samples[0][static_cast<size_t>(i)];
    }

    // --- Legacy path ---
    AudioCodecFilter_4X_PolyphaseFIR<SampleType> legacyFilter;
    AudioCodecCompressor_8BitVbrDelta<SampleType> legacyCompressor;
    std::vector<SampleType> legacyDecoded;
    PathMetrics legacyMetrics = runPath(
        "Legacy 8-bit / 12 kHz",
        legacyFilter,
        legacyCompressor,
        testFile,
        8,
        legacyDecoded,
        outFolder + stem + "_legacy.vbi");

    // --- HD path ---
    AudioCodecFilter_2X_PolyphaseFIR<SampleType> hdFilter;
    AudioCodecCompressor_12BitVbrDelta<SampleType> hdCompressor;
    std::vector<SampleType> hdDecoded;
    PathMetrics hdMetrics = runPath(
        "HD 12-bit / 24 kHz",
        hdFilter,
        hdCompressor,
        testFile,
        16,
        hdDecoded,
        outFolder + stem + "_hd.vbi");

    if (legacyMetrics.decodeMismatchPackets < 0 || hdMetrics.decodeMismatchPackets < 0) {
        return -1;
    }

    const int processed = std::min(legacyMetrics.processedSamples, hdMetrics.processedSamples);

    // Residuals (aligned). Legacy 4X FIR cascade delay is larger than 2X.
    std::vector<SampleType> legacyResidual;
    std::vector<SampleType> hdResidual;
    PathMetrics legacyQuality = analyseAndBuildResidual(
        allInput.data(), legacyDecoded.data(), processed, 512, &legacyResidual);
    PathMetrics hdQuality = analyseAndBuildResidual(
        allInput.data(), hdDecoded.data(), processed, 192, &hdResidual);

    legacyMetrics.snrDb = legacyQuality.snrDb;
    legacyMetrics.inputRms = legacyQuality.inputRms;
    legacyMetrics.residualRms = legacyQuality.residualRms;
    legacyMetrics.delaySamples = legacyQuality.delaySamples;

    hdMetrics.snrDb = hdQuality.snrDb;
    hdMetrics.inputRms = hdQuality.inputRms;
    hdMetrics.residualRms = hdQuality.residualRms;
    hdMetrics.delaySamples = hdQuality.delaySamples;

    printf("\n=== Writing listening WAVs (same rate/bit depth as source) ===\n");

    // Copy source into the compare folder for convenient one-folder audition.
    const std::string srcCopyPath = outFolder + stem + "_source.wav";
    if (!saveMonoWav(srcCopyPath, allInput.data(), numSamples, inputBitDepth, inputSampleRate)) {
        return -1;
    }

    const std::string legacyWav = outFolder + stem + "_legacy_decoded.wav";
    const std::string hdWav = outFolder + stem + "_hd_decoded.wav";
    if (!saveMonoWav(legacyWav, legacyDecoded.data(), numSamples, inputBitDepth, inputSampleRate)) {
        return -1;
    }
    if (!saveMonoWav(hdWav, hdDecoded.data(), numSamples, inputBitDepth, inputSampleRate)) {
        return -1;
    }

    const std::string legacyResWav = outFolder + stem + "_legacy_residual.wav";
    const std::string hdResWav = outFolder + stem + "_hd_residual.wav";
    if (!saveMonoWav(legacyResWav, legacyResidual.data(), processed, inputBitDepth, inputSampleRate)) {
        return -1;
    }
    if (!saveMonoWav(hdResWav, hdResidual.data(), processed, inputBitDepth, inputSampleRate)) {
        return -1;
    }

    // Short text summary for quick reference without re-running.
    const std::string summaryPath = outFolder + stem + "_summary.txt";
    {
        std::ofstream summary(summaryPath, std::ios::trunc);
        if (!summary.is_open()) {
            std::cerr << "Error: Unable to write summary: " << summaryPath << std::endl;
            return -1;
        }
        summary << "Listening A/B comparison\n";
        summary << "Source: " << testFilename << "\n";
        summary << "Format: " << inputBitDepth << "-bit, " << inputSampleRate << " Hz, "
                << inputLengthSeconds << " s\n";
        summary << "Processed samples: " << processed << " ("
                << (1000.0 * processed / SampleRate) << " ms)\n\n";

        summary << "Legacy (4X FIR + 8-bit VBR, 12 kHz internal)\n";
        summary << "  Decoded WAV:  " << legacyWav << "\n";
        summary << "  Residual WAV: " << legacyResWav << "\n";
        summary << "  Compressed:   " << (outFolder + stem + "_legacy.vbi")
                << " (" << legacyMetrics.compressedBytes << " bytes)\n";
        summary << "  SNR:          " << legacyMetrics.snrDb << " dB\n";
        summary << "  Residual RMS: " << legacyMetrics.residualRms << "\n";
        summary << "  Align delay:  " << legacyMetrics.delaySamples << " samples\n";
        summary << "  Decode mismatches: " << legacyMetrics.decodeMismatchPackets << "\n\n";

        summary << "HD (2X FIR + 12-bit VBR, 24 kHz internal)\n";
        summary << "  Decoded WAV:  " << hdWav << "\n";
        summary << "  Residual WAV: " << hdResWav << "\n";
        summary << "  Compressed:   " << (outFolder + stem + "_hd.vbi")
                << " (" << hdMetrics.compressedBytes << " bytes)\n";
        summary << "  SNR:          " << hdMetrics.snrDb << " dB\n";
        summary << "  Residual RMS: " << hdMetrics.residualRms << "\n";
        summary << "  Align delay:  " << hdMetrics.delaySamples << " samples\n";
        summary << "  Decode mismatches: " << hdMetrics.decodeMismatchPackets << "\n\n";

        summary << "SNR delta (HD - Legacy): "
                << (hdMetrics.snrDb - legacyMetrics.snrDb) << " dB\n";
        summary << "Size ratio (HD / Legacy compressed): "
                << (static_cast<double>(hdMetrics.compressedBytes) /
                    std::max(1u, legacyMetrics.compressedBytes))
                << "\n";
        summary << "\nAudition tip: open *_source.wav, *_legacy_decoded.wav, and\n"
                << "*_hd_decoded.wav in a DAW or audio editor and A/B the same region.\n";
    }
    printf("  Wrote %s\n", summaryPath.c_str());

    printf("\n=== Comparison summary ===\n");
    printf("Legacy: SNR=%0.2f dB, residual RMS=%0.2f, delay=%d, compressed=%u bytes, mismatches=%d\n",
           legacyMetrics.snrDb, legacyMetrics.residualRms, legacyMetrics.delaySamples,
           legacyMetrics.compressedBytes, legacyMetrics.decodeMismatchPackets);
    printf("HD:     SNR=%0.2f dB, residual RMS=%0.2f, delay=%d, compressed=%u bytes, mismatches=%d\n",
           hdMetrics.snrDb, hdMetrics.residualRms, hdMetrics.delaySamples,
           hdMetrics.compressedBytes, hdMetrics.decodeMismatchPackets);
    printf("SNR delta (HD - Legacy): %+.2f dB\n",
           hdMetrics.snrDb - legacyMetrics.snrDb);
    printf("Compressed size ratio (HD/Legacy): %.2fx\n",
           static_cast<double>(hdMetrics.compressedBytes) /
               static_cast<double>(std::max(1u, legacyMetrics.compressedBytes)));

    printf("\nListening files in: %s\n", outFolder.c_str());
    printf("  %s_source.wav\n", stem.c_str());
    printf("  %s_legacy_decoded.wav\n", stem.c_str());
    printf("  %s_hd_decoded.wav\n", stem.c_str());
    printf("  %s_legacy_residual.wav\n", stem.c_str());
    printf("  %s_hd_residual.wav\n", stem.c_str());
    printf("  %s_summary.txt\n", stem.c_str());

    const int mismatches =
        legacyMetrics.decodeMismatchPackets + hdMetrics.decodeMismatchPackets;
    printf("\nListening harness done.\n\n");
    return (mismatches == 0) ? 0 : 1;
}
