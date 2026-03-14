#include "../Codecs/AudioCodec.hpp"
#include "../Codecs/Filters/AudioCodecFilter_4X.hpp"
#include "../Codecs/Filters/AudioCodecFilter_4X_PolyphaseFIR.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_8BitScaled.hpp"
#include "../Codecs/Compressors/AudioCodecCompressor_8BitVbrDelta.hpp"
// #include "../Codecs/Compressors/AudioCodecCompressor_8BitANSDelta.hpp"
#include "../Codecs/Squelchers/AudioCodecSquelcher_Basic.hpp"

#include "../Codecs/Utility/AudioFileUtils.hpp"
#include "TestData/make_test_audio.hpp"
#include "AudioFile.h"

// Native audio sample data type - must match the test data WAV files' sample type
#define SampleType    AudioS16

// For testing, we'll use the common 48kHz sample rate.
// Since audio packets will be 256 samples, at 12kHz downsampled rate, we'll use 1024 input samples.
// This is then a good test for a realistic native audio input for one audio packet.
#define SampleRate      48000
#define NumInputSamples 1024

#define TestFrequency 1753
#define TestAmplitude 0.95f

const std::string testFileFolder = "../Tests/TestData/SpeechFiles/";

const std::string testFile1 = testFileFolder + "0016_000013.wav";
const std::string testFile2 = testFileFolder + "0016_000022.wav";
const std::string testFile3 = testFileFolder + "0016_000255.wav";
const std::string testFile4 = testFileFolder + "0016_000350.wav";

int main()
{
    printf("Testing AudioFile WAV file compression.\n");

    //std::filesystem::path cwd = std::filesystem::current_path();
    //std::cout << "Current working directory: " << cwd << std::endl;

    AudioFile<SampleType> testFile;
    AudioFile<SampleType> outFile;

    // WAV Filename for testing
    std::string testFilename = testFile4;

    // Prepare compressed output file
    std::string compressedFilename = stripFileExtension(testFilename).append("_comp.vbi");
    std::ofstream compressedFile (compressedFilename, std::ios::trunc | std::ios::binary);
    if (!compressedFile.is_open())
    {
        std::cerr << "Error: Unable to open output file at " << compressedFilename << std::endl; 
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
    codec.setFilter(codecFilter);

    // Set Compressor
    AudioCodecCompressor_8BitVbrDelta<SampleType> codecCompressor;
//    AudioCodecCompressor_8BitScaled<SampleType> codecCompressor;
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
    }

    // Encode audio into compressed buffer
    uint32_t outCompBufferWritten = compBufferSize;
    codec->encode(inputBuffer, NumInputSamples, compBuffer, &outCompBufferWritten);
    printf("Filtered, decimated and encoded audio length: %d bytes\r\n", outCompBufferWritten);

    // Decode compressed audio into output buffer
    uint32_t outOutputSamplesWritten = NumInputSamples;
    codec->decode(compBuffer, outCompBufferWritten, outputBuffer, &outOutputSamplesWritten);
    printf("Decoded and expanded audio length: %lu bytes (%d samples, %lu bytes per sample)\r\n", outOutputSamplesWritten * sizeof(SampleType), outOutputSamplesWritten, sizeof(SampleType));

    // Calculate compression ratio
    float compRatio32 = (float)outCompBufferWritten / (float)ioBufferSize;
    printf("\nCompression Ratio: %0.1f:1 or %.1f %% (compared to 32-bit floating point)\r\n", (1.0f/compRatio32), compRatio32 * 100);

    float compRatio16 = (float)outCompBufferWritten / (float)(NumInputSamples/2 + 1);
    printf("Compression Ratio: %0.1f:1 or %.1f %% (compared to 16-bit PCM)\r\n", (1.0f/compRatio16), compRatio16 * 100);

    float compRatio8 = (float)outCompBufferWritten / (float)(NumInputSamples/4 + 1);
    printf("Compression Ratio: %0.1f:1 or %.1f %% (compared to 8-bit scaled)\r\n", (1.0f/compRatio8), compRatio8 * 100);

    // Calculate data rate
    float dataRate = (float)outCompBufferWritten / ((float)NumInputSamples/(float)SampleRate);
    printf("Compressed Data Rate: %0.1f kB/sec or %0.0f kbps\r\n\n", dataRate / 1024.0, dataRate / 128.0);

    // Report histogram
    AudioCodecCompressor_8BitVbrDelta<SampleType> *compressor = (AudioCodecCompressor_8BitVbrDelta<SampleType>*)codec->m_compressor;
    uint32_t numSamples = 0;
    for (int i=-127; i<128; i++) {
        uint8_t count = compressor->m_deltaHistogram[(uint8_t)i];
        printf("Histogram[%d] = %d\n", i, count);
        numSamples += count;
    }
    printf("Delta Histogram sanity check: numDeltaSamples = %d\n\n", numSamples);

    // Quick & Dirty error calculation - Just run through comparing input and output buffer contents
    double diff_acc = 0.0;
    double scale_acc = 0.0;
    for (int i=0; i<NumInputSamples-1; i++) {
        // Offset here between compared samples is to account for the tiny delay introduced by the
        // interpolation process when expanding back to full sample rate. Wouldn't be fair otherwise.
        double inputSample = (double)inputBuffer[i];
        double outputSample = (double)outputBuffer[i+1];
        double diff = fabs(inputSample - outputSample);
        diff_acc += diff;
        double scale = (fabs(inputSample) < 0.001) ? 1.0 : fabs(outputSample / inputSample);
//        printf("scale: %0.2f ... ", scale);
        scale_acc += scale;

        // Log some of the last samples for visible verification
        if (i > ((NumInputSamples*6)/8)) {
//            printf("Test(raw): InputSample[%d] = %0.3f  \tOutputSample[%d] = %0.3f  \tDiff = %0.3f  \tScale = %0.3f\r\n", i, inputSample, i+1, outputSample, diff, scale);
        }
    }
    double avg_err = (diff_acc / (NumInputSamples-1) / TestAmplitude) * 100.0;
    printf("\nAverage differential error, input vs output: +/- %.3f %%\r\n", avg_err);
    double scale_err = (scale_acc / (NumInputSamples-1));
    printf("Average scale error, input vs output: %.2f %% (raw: %0.2f)\r\n", scale_err * 100.0 - 100.0, scale_err);

    diff_acc = 0.0;
    // Redo the differential error after correction for scale error.
    // This gives us a better idea of the audio quality, since even a perfectly faithful signal
    // that is scaled down (due to filter roll-off, for example) will create an artificial 
    // differential error that doesn't really represent a loss of quality.
    double scale_adj = scale_err;//sqrt(scale_err);
    for (int i=0; i<NumInputSamples-1; i++) {
        // Offset here between compared samples is to account for the tiny delay introduced by the
        // interpolation process when expanding back to full sample rate. Wouldn't be fair otherwise.
        double inputSample = (double)inputBuffer[i];
        double outputSample = (double)outputBuffer[i+1] / scale_adj; // Divide by scale_err to get closer comparison
        double diff = fabs(inputSample - outputSample);
        diff_acc += diff;     
        // Log some of the last samples for visible verification
        if (i > ((NumInputSamples*6)/8)) {
    //        printf("Test(scaleCorrected): InputSample[%d] = %0.3f  \tOutputSample[%d] = %0.3f  \tDiff = %0.3f\r\n", i, inputSample, i+1, outputSample, diff);
        }
    }

    // Of course, this doesn't entirely improve the differential error, becuase the filter also
    // introduces a frequency-dependent phase shift, which also makes samples less comparable.
    avg_err = (diff_acc / (NumInputSamples-1) / TestAmplitude) * 100.0;
    printf("Scale-corrected average differential error, input vs output: +/- %.3f %%\r\n", avg_err);
    
    // Cleanup
    delete synth;
    free(compBuffer);
    free(inputBuffer);
    free(outputBuffer);
    delete codec;

    printf("\nAll done.\r\n\n");

    return 0;
}

*/