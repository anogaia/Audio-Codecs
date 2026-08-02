#include "AudioCodecCompressor_12BitScaled.hpp"

// 12-Bit Scaled Audio Compressor
// Analogous to the 8-bit scaled compressor, but preserves a 12-bit dynamic range.
// The audio is first referenced to an intermediate signed 16-bit representation,
// then scaled into signed 12-bit values (-2047 .. +2047). The unsigned 8-bit scaling
// value is stored as the first byte of the compressed packet (same scale encoding
// as 8BitScaled). Each subsequent sample is stored as a little-endian int16_t
// whose magnitude uses at most 12 significant bits.

static constexpr int16_t k12BitMaxSample = 2047;

// Compress a buffer of native samples.
template <class AudioSampleType>
void AudioCodecCompressor_12BitScaled<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {

    // Get the full scale value for our sample type
    double fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    // First, we need to detect the maximum amplitude in our input audio
    AudioSampleType maxSample = (AudioSampleType)0;
    for (int i=0; i<numInputSamples; i++) {
        AudioSampleType inputSample = std::abs(inputSamples[i]);
        maxSample = std::max(inputSample, maxSample);
    }

    printf("12BitScaled: Max Sample Value: %0.3f\r\n", (float)maxSample);

    // Same scale-byte calculation as 8BitScaled (top 8 bits of the 16-bit amplitude).
    double normMaxSample = maxSample / fullScale;
    double maxSample16BitSigned = normMaxSample * 32767.0;
    uint16_t top8MaxSample = (uint16_t)maxSample16BitSigned >> 7;
    // Silent packet or any packet below 1/256th of full scale could generate 0 here, so make it 1 if so.
    top8MaxSample = std::max(top8MaxSample, (uint16_t)1);
    uint8_t scaleValue = (uint8_t)(top8MaxSample);

    // Zero the out parameter that tracks how many bytes are being written out
    *outNumBytesOutputData = 0;
    // Write out scale value as first byte
    *outputBytes++ = scaleValue;
    (*outNumBytesOutputData)++; // inc bytes written

    // Map into +/-2047 using the same scale byte semantics as 8-bit (which maps to +/-127),
    // scaled by (2047/127) to preserve 12-bit dynamic range.
    double downScale = 32767.0 / fullScale / (scaleValue + 1.0) * ((double)k12BitMaxSample / 127.0);

    printf("12BitScaled: Max int16 Sample Value after downscaling: %d\n", (int16_t)(maxSample * downScale));

    for (int i=0; i<numInputSamples; i++) {
        int32_t sample12 = (int32_t)(inputSamples[i] * downScale);
        // Clamp to 12-bit signed range (leave -2048 free for VBR end-of-run use on deltas)
        if (sample12 > k12BitMaxSample) sample12 = k12BitMaxSample;
        if (sample12 < -k12BitMaxSample) sample12 = -k12BitMaxSample;
        int16_t outSample = (int16_t)sample12;
        // Little-endian store
        *outputBytes++ = (AudioBytes)(outSample & 0xFF);
        *outputBytes++ = (AudioBytes)((outSample >> 8) & 0xFF);
        (*outNumBytesOutputData) += 2;
    }
}

// Decompress a buffer of compressed samples to native samples.
// Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
template <class AudioSampleType>
void AudioCodecCompressor_12BitScaled<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // Get the full scale value for our sample type
    AudioSampleType fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    // Read the scale value from the first byte of the stream. Add 1 to get the actual scaling value.
    double scaleValue = (*inputData++) + 1.0;

    // Zero out parameter for num samples written
    *outNumOutputSamples = 0;

    // Inverse of the 12-bit downScale above.
    double upScale = (fullScale / 32767.0) * scaleValue * (127.0 / (double)k12BitMaxSample);

    // Remaining bytes are little-endian int16 samples.
    uint32_t numSamples = (numBytesInputData > 0) ? (numBytesInputData - 1) / 2 : 0;
    for (uint32_t i=0; i<numSamples; i++) {
        int16_t inputSample = (int16_t)(inputData[0] | (inputData[1] << 8));
        inputData += 2;
        AudioSampleType outputSample = (AudioSampleType)(inputSample * upScale);
        *outputSamples++ = outputSample;
        (*outNumOutputSamples)++; // Inc output samples written
    }
}

// Utility functions to derive buffer requirements.

template <class AudioSampleType>
uint32_t AudioCodecCompressor_12BitScaled<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    // 1 scale byte + 2 bytes per 12-bit sample stored as int16.
    return (numSamples * 2) + 1;
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_12BitScaled<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    // This is simple. It's just the number of samples multiplied by the audio output sample size.
    return sizeof(AudioSampleType) * numSamples;
}


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecCompressor_12BitScaled<AudioF32>;
template class AudioCodecCompressor_12BitScaled<AudioF64>;
template class AudioCodecCompressor_12BitScaled<AudioS8>;
template class AudioCodecCompressor_12BitScaled<AudioS16>;
template class AudioCodecCompressor_12BitScaled<AudioS32>;
