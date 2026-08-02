#include "../Codecs/AudioCodec.hpp"
#include "anog_codec_io.hpp"
#include "AudioFile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef AUDIO_CODECS_TEST_HD
#include "../Codecs/Filters/AudioCodecFilter_2X_PolyphaseFIR.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_12BitVbrDelta.hpp"
#include "../Codecs/Utility/AudioCodecUtils.hpp"
#endif

using SampleType = AnogCodec::Sample;

const std::string testFileFolder = "../Tests/TestData/SpeechFiles/";
const std::string testFile4 = testFileFolder + "0016_000350.wav";

int main() {
    printf("Testing ANOG encode/decode on a speech WAV.\n");

    AudioFile<SampleType> testFile;
    const std::string testFilename = testFile4;
    if (!testFile.load(testFilename)) {
        std::cerr << "Error: Unable to load " << testFilename << "\n";
        return -1;
    }

    const int inputBitDepth = testFile.getBitDepth();
    const int inputSampleRate = testFile.getSampleRate();
    const float inputLengthSeconds = testFile.getLengthInSeconds();
    const int numSamples = testFile.getNumSamplesPerChannel();
    const int numChannels = testFile.getNumChannels();
    printf("Test File Bit Depth: %d, Sample Rate: %d, Channels: %d, Length: %0.2f s.\n",
           inputBitDepth, inputSampleRate, numChannels, inputLengthSeconds);

    std::vector<std::vector<SampleType>> channels;
    channels.resize(size_t(numChannels));
    for (int c = 0; c < numChannels; c++) {
        channels[size_t(c)].resize(size_t(numSamples));
        for (int i = 0; i < numSamples; i++) {
            channels[size_t(c)][size_t(i)] = testFile.samples[size_t(c)][size_t(i)];
        }
    }

    const uint16_t frame_ms = 100;
    const std::string compressedFilename = testFilename.substr(0, testFilename.find_last_of('.')) + "_comp.anog";

    try {
        AnogCodec::encode_wav_channels_to_anog(channels, uint32_t(inputSampleRate), frame_ms,
                                               compressedFilename);
    } catch (const std::exception &ex) {
        std::cerr << "Encode failed: " << ex.what() << "\n";
        return -1;
    }

    std::ifstream measure(compressedFilename, std::ios::binary | std::ios::ate);
    const auto compFileLength = measure.tellg();
    measure.close();

    const double pcmBytes =
        double(numSamples) * double(numChannels) * double(sizeof(SampleType));
    const double ratio = pcmBytes / double(compFileLength);
    const double kbps = (double(compFileLength) * 8.0 / inputLengthSeconds) / 1000.0;

    printf("Saved compressed output: %s\n", compressedFilename.c_str());
    printf("Compressed size: %lld bytes (%0.1f kB)\n",
           static_cast<long long>(compFileLength), double(compFileLength) / 1024.0);
    printf("Compression vs 16-bit PCM: %0.2f:1\n", ratio);
    printf("Average bitrate: %0.1f kbps (frame_ms=%u)\n", kbps, unsigned(frame_ms));

    Anog::Header hdr;
    std::vector<std::vector<SampleType>> decoded;
    try {
        AnogCodec::decode_anog_to_channels(compressedFilename, hdr, decoded);
    } catch (const std::exception &ex) {
        std::cerr << "Decode failed: " << ex.what() << "\n";
        return -1;
    }

    AudioFile<SampleType> outFile;
    outFile.setBitDepth(16);
    outFile.setSampleRate(int(hdr.pcm_sample_rate));
    outFile.setNumChannels(int(hdr.channels));
    outFile.setNumSamplesPerChannel(int(hdr.pcm_total_samples));
    for (uint8_t c = 0; c < hdr.channels; c++) {
        for (uint64_t i = 0; i < hdr.pcm_total_samples; i++) {
            outFile.samples[c][size_t(i)] = decoded[c][size_t(i)];
        }
    }

    const std::string outputFilename = testFilename.substr(0, testFilename.find_last_of('.')) + "_proc.wav";
    outFile.save(outputFilename);
    printf("Saved decompressed WAV: %s\n", outputFilename.c_str());
    printf("ANOG frames: %u, compressed samples/ch: %llu\n", hdr.seek_entry_count,
           static_cast<unsigned long long>(hdr.compressed_total_samples));

#ifdef AUDIO_CODECS_TEST_HD
    // Optional tiny HD building-block smoke (does not replace tests_hd / tests_listen).
    // Enable with: cmake -DAUDIO_CODECS_TEST_HD=ON ..
    {
        printf("\n--- HD building-block smoke test (2X FIR + 12-bit VBR) ---\n");
        constexpr uint32_t kSampleRate = 48000;
        AudioCodec<SampleType> hdCodec;
        AudioCodecFilter_2X_PolyphaseFIR<SampleType> hdFilter;
        AudioCodecCompressor_12BitVbrDelta<SampleType> hdCompressor;
        hdCodec.setFilter(hdFilter);
        hdCodec.setCompressor(hdCompressor);

        const uint32_t hdInputSamples = 512;
        SampleType *hdIn = (SampleType*)malloc(hdInputSamples * sizeof(SampleType));
        SampleType *hdOut = (SampleType*)malloc(hdInputSamples * sizeof(SampleType));
        uint32_t hdCompSize = hdCodec.getMaxEncodedBytes(hdInputSamples);
        uint8_t *hdComp = (uint8_t*)malloc(hdCompSize);

        for (uint32_t i = 0; i < hdInputSamples; i++) {
            float t = (float)i / (float)kSampleRate;
            hdIn[i] = (SampleType)(sin(2.0 * M_PI * 440.0 * t) * 0.5f *
                                   AudioCodecUtils::fullScaleValue<SampleType>());
        }

        uint32_t written = hdCompSize;
        hdCodec.encode(hdIn, hdInputSamples, hdComp, &written);
        uint32_t decodedN = hdInputSamples;
        hdCodec.decode(hdComp, written, hdOut, &decodedN);
        printf("HD smoke: encoded %u bytes, decoded %u samples (expected %u)\n", written, decodedN,
               hdInputSamples);

        free(hdIn);
        free(hdOut);
        free(hdComp);
    }
#endif

    printf("\nAll done.\n");
    return 0;
}
