#include "../Codecs/AudioCodec.hpp"
#include "../Codecs/Filters/AudioCodecFilter_4X.hpp"
#include "../Codecs/Filters/AudioCodecFilter_4X_PolyphaseFIR.hpp"
#include "../Codecs/Filters/AudioCodecFilter_2X_PolyphaseFIR.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_8BitScaled.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_8BitVbrDelta.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_12BitScaled.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_12BitVbrDelta.hpp"
// #include "../Codecs/Compressors/AudioCodecCompressor_8BitANSDelta.hpp"
#include "../Codecs/Squelchers/AudioCodecSquelcher_Basic.hpp"

#include "../Codecs/Utility/AudioFileUtils.hpp"
#include "TestData/make_test_audio.hpp"
#include "AudioFile.h"

#include <cmath>

// Native audio sample data type - must match the test data WAV files' sample type
#define SampleType    AudioS16

// For testing, we'll use the common 48kHz sample rate.
// Since audio packets will be 256 samples, at 12kHz downsampled rate, we'll use 1024 input samples.
// This is then a good test for a realistic native audio input for one audio packet.
#define SampleRate      48000
#define NumInputSamples 1024
#include "anog_codec_io.hpp"
#include "AudioFile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

    // Load WAV file
    testFile.load(testFilename);

    int inputBitDepth = testFile.getBitDepth();
    int inputSampleRate = testFile.getSampleRate();
    float inputLengthSeconds = testFile.getLengthInSeconds();
    printf("Test File Bit Depth: %d, Sample Rate: %d, Length: %0.2f (seconds).\n", inputBitDepth, inputSampleRate, inputLengthSeconds);

    // Set output file to same format as the file being tested
    outFile.setBitDepth(inputBitDepth);
    outFile.setSampleRate(inputSampleRate);


    // Audio Codec object
    AudioCodec<SampleType> codec;

    // Set Filter
    AudioCodecFilter_4X_PolyphaseFIR<SampleType> codecFilter;
//    AudioCodecFilter_4X<SampleType> codecFilter;
//    AudioCodecFilter_2X_PolyphaseFIR<SampleType> codecFilter;  // HD: 48 kHz <-> 24 kHz
    codec.setFilter(codecFilter);

    // Set Compressor
    AudioCodecCompressor_8BitVbrDelta<SampleType> codecCompressor;
//    AudioCodecCompressor_8BitScaled<SampleType> codecCompressor;
//    AudioCodecCompressor_12BitVbrDelta<SampleType> codecCompressor;  // HD: 12-bit VBR delta
    codec.setCompressor(codecCompressor);

    // Set Squelcher
    AudioCodecSquelcher_Basic<SampleType> codecSquelcher;
    codec.setSquelcher(codecSquelcher);

    // Create input & output audio buffers
    uint32_t ioBufferSize = NumInputSamples * sizeof(SampleType);
    SampleType *inputBuffer = (SampleType*)malloc(ioBufferSize);
    SampleType *outputBuffer = (SampleType*)malloc(ioBufferSize);
    printf("Created input audio buffer, length: %d bytes (%d samples, %lu bytes per sample)\r\n", ioBufferSize, NumInputSamples, sizeof(SampleType));

    // Create compressed audio buffer
    uint32_t compBufferSize = codec.getMaxEncodedBytes(NumInputSamples);
    uint8_t *compBuffer = (uint8_t*)malloc(compBufferSize);
    printf("Created encoded audio buffer, length: %d bytes\r\n", compBufferSize);

    // Get number of samples in input file
    int numSamples = testFile.getNumSamplesPerChannel();
    // Set the audio output file to the same length, with 1 channel
    outFile.setAudioBufferSize(1, numSamples);

    float compRatio8_acc = 0.0f;
    float compBufferWritten_acc = 0.0f;
    int numPackets = 0;
    uint32_t compFileLength = 0;

    // Write out VBIF header for compressed file
    AudioFileUtils::vbif_header compHeader;
    uint32_t frameLength = codecFilter.getDecimatedSamples(NumInputSamples);
    compHeader = AudioFileUtils::Make_VBIF_Header(8, inputSampleRate, frameLength);
    compressedFile.write (reinterpret_cast<const char*> (&compHeader), sizeof(compHeader));
    compFileLength += sizeof(compHeader);


    if (numSamples > NumInputSamples) {
        for (int offset=0; offset<(numSamples - (NumInputSamples-1)); offset+=NumInputSamples) {

            // Read 1 packet of audio data from input file
            for (int i=0; i<NumInputSamples; i++) {
                SampleType currentSample = testFile.samples[0][i+offset];
                inputBuffer[i] = currentSample;
            }

            printf("\n\nStarting to encode packet %d at offset %d in input file.\n", numPackets, offset);

            // Encode audio into compressed buffer
            uint32_t outCompBufferWritten = compBufferSize;
            codec.encode(inputBuffer, NumInputSamples, compBuffer, &outCompBufferWritten);
            printf("Filtered, decimated and encoded audio length: %d bytes\r\n", outCompBufferWritten);

            // Append this packet to the compressed output file
            compressedFile.write (reinterpret_cast<const char*> (compBuffer), outCompBufferWritten);
            compFileLength += outCompBufferWritten;

            // Decode compressed audio into output buffer
            uint32_t outOutputSamplesWritten = NumInputSamples;
            codec.decode(compBuffer, outCompBufferWritten, outputBuffer, &outOutputSamplesWritten);
            if (outOutputSamplesWritten != NumInputSamples) printf("Error: decoded %d samples which is not the same as the %d input samples that went in.\n", outOutputSamplesWritten, NumInputSamples);
            printf("Decoded and expanded audio length: %lu bytes (%d samples, %lu bytes per sample)\r\n", outOutputSamplesWritten * sizeof(SampleType), outOutputSamplesWritten, sizeof(SampleType));

            // Write 1 packet of audio data to output file
            for (int i=0; i<NumInputSamples; i++) {
                SampleType currentSample = outputBuffer[i];
                outFile.samples[0][i+offset] = currentSample;
            }

            // Calculate compression ratio
            float compRatio32 = (float)outCompBufferWritten / (float)ioBufferSize;
//            printf("\nCompression Ratio: %0.1f:1 or %.1f %% (compared to 32-bit floating point)\r\n", (1.0f/compRatio32), compRatio32 * 100);

            float compRatio16 = (float)outCompBufferWritten / (NumInputSamples/2.0f + 1.0f);
//            printf("Compression Ratio: %0.1f:1 or %.1f %% (compared to 16-bit PCM)\r\n", (1.0f/compRatio16), compRatio16 * 100);

            float compRatio8 = (float)outCompBufferWritten / (NumInputSamples/4.0f + 1.0f);
            printf("Compression Ratio: %0.2f:1 or %.1f %% (compared to 8-bit scaled)\r\n", (1.0f/compRatio8), compRatio8 * 100);

            compRatio8_acc += compRatio8;
            compBufferWritten_acc += outCompBufferWritten;
            numPackets++;
        }
    } else {
        printf("Can't encode this file - too few samples. Must have at least %d samples.\n", NumInputSamples);
    }

    float compRatio8_avg = compRatio8_acc / numPackets;
    printf("\nAverage compression ratio over %d packets: %0.2f:1 or %.1f %% (compared to 8-bit scaled)\n", numPackets, (1.0f/compRatio8_avg), compRatio8_avg * 100);

    // Calculate average data rate
    float avgCompBufferWritten = compBufferWritten_acc / numPackets;
    float dataBitRate = avgCompBufferWritten / ((float)NumInputSamples/(float)SampleRate);
    printf("\nAverage Compressed Data Rate: %0.1f kB/sec or %0.0f kbps\r\n\n", dataBitRate / 1024.0, dataBitRate / 128.0);

    // Close compressed output file
    compressedFile.close();
    printf("Final compressed audio file length: %d bytes (%0.1f kB)\n", compFileLength, compFileLength / 1024.0f);
    printf("Saved compressed output file as: %s\n", compressedFilename.c_str());

    // Save output file
    std::string outputFilename = stripFileExtension(testFilename).append("_proc.wav");
    outFile.save(outputFilename);

    printf("\nFinal decompressed audio file length: %d bytes (%0.1f kB) - should be same as input file.\n", outFile.savedLength, outFile.savedLength / 1024.0f);
    printf("Saved decompressed output file as: %s\n", outputFilename.c_str());

    free(inputBuffer);
    free(outputBuffer);
    free(compBuffer);

    // Minimal HD building-block smoke test (does not change the default 8-bit / 12 kHz path above).
    // Enable with: cmake -DAUDIO_CODECS_TEST_HD=ON ..
#ifdef AUDIO_CODECS_TEST_HD
    {
        printf("\n--- HD building-block smoke test (2X FIR + 12-bit VBR) ---\n");
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

        for (uint32_t i=0; i<hdInputSamples; i++) {
            float t = (float)i / (float)SampleRate;
            hdIn[i] = (SampleType)(sin(2.0 * M_PI * 440.0 * t) * 0.5f * AudioCodecUtils::fullScaleValue<SampleType>());
        }

        uint32_t written = hdCompSize;
        hdCodec.encode(hdIn, hdInputSamples, hdComp, &written);
        uint32_t decoded = hdInputSamples;
        hdCodec.decode(hdComp, written, hdOut, &decoded);
        printf("HD smoke: encoded %u bytes, decoded %u samples (expected %u)\n", written, decoded, hdInputSamples);

        free(hdIn); free(hdOut); free(hdComp);
    }
#endif

    printf("\nAll done.\n\n");

    return 0;
}

/*
int main()
{
    printf("Testing AudioFile WAV file reading.\n");

    AudioFile<SampleType> testFile;

    testFile.load(testFile3);

    printf("Test File Bit Depth: %d, Sample Rate: %d, Length: %0.2f (seconds).\n", testFile.getBitDepth(), testFile.getSampleRate(), testFile.getLengthInSeconds());

    printf("\nAll done.\n");

    return 0;
}
*/

/*
int main()
{
    printf("\r\nAudio Compression Test:\r\nTest Frequency: %d Hz, Test Amplitude: %0.2f\n\n", TestFrequency, TestAmplitude);

    // Create Codec
    AudioCodec<SampleType> *codec = new AudioCodec<SampleType>();
    // Set Filter and Compressor
    codec->setFilter(new AudioCodecFilter_4X<SampleType>());
//    codec->setCompressor(new AudioCodecCompressor_8BitANSDelta<SampleType>());
    codec->setCompressor(new AudioCodecCompressor_8BitVbrDelta<SampleType>());
//    codec->setCompressor(new AudioCodecCompressor_8BitScaled<SampleType>());

    // Create input & output audio buffers
    uint32_t ioBufferSize = NumInputSamples * sizeof(SampleType);
    SampleType *inputBuffer = (SampleType*)malloc(ioBufferSize);
    SampleType *outputBuffer = (SampleType*)malloc(ioBufferSize);
    printf("Created input audio buffer, length: %d bytes (%d samples, %lu bytes per sample)\r\n", ioBufferSize, NumInputSamples, sizeof(SampleType));

    // Create compressed audio buffer
    uint32_t compBufferSize = codec->getMaxEncodedBytes(NumInputSamples);
    uint8_t *compBuffer = (uint8_t*)malloc(compBufferSize);
    printf("Created encoded audio buffer, length: %d bytes\r\n", compBufferSize);

    // Fill input buffer with some audio data
    SynthesiseAudio<SampleType> *synth = new SynthesiseAudio<SampleType>();
//    synth->sineWave(TestFrequency, SampleRate, TestAmplitude, inputBuffer, NumInputSamples);
    synth->noise(SampleRate, TestAmplitude, inputBuffer, NumInputSamples);

    // Try squelching a block of the data in the middle, to simulate a pause.
    for (int i=0; i<NumInputSamples; i++) { 
        double distanceFromMiddle = fabs(i - (NumInputSamples/2));
        double normalised = distanceFromMiddle / NumInputSamples;
        double scale = normalised * normalised;
        inputBuffer[i] *= scale;
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
    printf("\nAll done.\n");
    return 0;
}
