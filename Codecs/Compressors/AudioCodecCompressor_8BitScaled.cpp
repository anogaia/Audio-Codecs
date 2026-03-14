#include "AudioCodecCompressor_8BitScaled.hpp"

// 8-Bit Scaled Audio Compressor
// The 8-bit audio is downsampled from the audio input type by first converting to 16-bit PCM.
// This is then scaled down to 8-bit and the unsigned 8-bit scaling value is stored in the compressed audio packet
// as the first byte.
// The rest of the compressed audio is simply the scaled 8-bit samples.

// Compress a buffer of native samples.
template <class AudioSampleType>
void AudioCodecCompressor_8BitScaled<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {
    
    // Get the full scale value for our sample type
    double fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    // First, we need to detect the maximum amplitude in our input audio
    AudioSampleType maxSample = (AudioSampleType)0;
    for (int i=0; i<numInputSamples; i++) {
        AudioSampleType inputSample = std::abs(inputSamples[i]);
//        printf("8BitScaled: Input Sample: %0.2f Abs Input Sample: %0.2f\r\n", (float)inputSamples[i], (float)inputSample);
        maxSample = std::max(inputSample, maxSample);
    }

    printf("8BitScaled: Max Sample Value: %0.3f\r\n", (float)maxSample);

    // Now we need to calculate the maximum scale value which keeps this maximum amplitude below +127 
    // when the input samples are divided by it.
    // Of course, reconstruction will then simply be multiplying the 8-bit signed values that result,
    // by the same scale value.
    // We effectively use an internal signed 16-bit representation here, so that the scale value is 
    // also an 8-bit value, although in this case the scale value is unsigned, since it is only a
    // uniform scaling value and does not ever need (or want) to invert the sign of sample values.
    
    // First, calculate what our maximum amplitude is, with respect to the full scale value.
    // Effectively, normalise maximum amplitude to a full scale of 1.0
    double normMaxSample = maxSample / fullScale;
//    printf("8BitScaled: Normalised Max Sample Value: %0.2f\r\n", (float)normMaxSample);
    // Now we can convert that value to the intermediate 16-bit signed value.
    double maxSample16BitSigned = normMaxSample * 32767.0;
//    printf("8BitScaled: maxSample16BitSigned Value: %0.2f\r\n", (float)maxSample16BitSigned);
    // Scale that to the value that makes +127 our maximum stored value.
    uint16_t top8MaxSample = (uint16_t)maxSample16BitSigned >> 7;
//    printf("8BitScaled: top8MaxSample Value: %d\r\n", top8MaxSample);
    // Silent packet or any packet below 1/256th of full scale could generate 0 here, so make it 1 if so.
    top8MaxSample = std::max(top8MaxSample, (uint16_t)1);
    uint8_t scaleValue = (uint8_t)(top8MaxSample);
//    printf("8BitScaled: Scale Value: %d\r\n", scaleValue);

    // Zero the out parameter that tracks how many bytes are being written out
    *outNumBytesOutputData = 0;
    // Write out scale value as first byte
    *outputBytes++ = scaleValue;
    (*outNumBytesOutputData)++; // inc bytes written
  
    // Now we can downscale our input samples and write them out.
    // Remember to increase scaleValue by 1 to get it back to the 1..256 range.
    // We also integrate the conversion to the intermediate 16-bit signed representation
    // and the scaling to the full scale value of the sample type here, so it becomes a 
    // single multiplication.
    double downScale = 32767.0 / fullScale / (scaleValue + 1.0);

    printf("8BitScaled: Max int8_t Sample Value after downscaling: %d\n", (int8_t)(maxSample * downScale));

//    printf("8BitScaled: downScale Value: %0.2f\r\n", (float)downScale);
    for (int i=0; i<numInputSamples; i++) {
        int16_t outputByte = (int16_t)(inputSamples[i] * downScale);
        int16_t unClampedOutputByte = outputByte;
        // Clamp to between -127 & +127
//        outputByte = std::min(outputByte, (int16_t)127);
//        outputByte = std::max(outputByte, (int16_t)-127);
        *outputBytes++ = (int8_t)outputByte;
//        printf("8BitScaled: outputByte unclamped Value: %d \toutputByte 8bit Value: %d\r\n", unClampedOutputByte, (int8_t)outputByte);
        (*outNumBytesOutputData)++; // inc bytes written
    }
}

// Decompress a buffer of compressed samples to native samples. 
// Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
template <class AudioSampleType>
void AudioCodecCompressor_8BitScaled<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // Get the full scale value for our sample type
    AudioSampleType fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    // Read the scale value from the first byte of the stream. Add 1 to get the actual scaling value.
    double scaleValue = (*inputData++) + 1.0;

    // Zero out parameter for num samples written
    *outNumOutputSamples = 0;

    // Calculate the upScale value we need to re-scale the downScaled values back to the 
    // full-scale of the sample type in use.
    double upScale = (fullScale / 32767.0) * scaleValue;
//    printf("8BitScaled: decoder upScale Value: %0.2f\r\n", (float)upScale);
    for (int i=0; i<(numBytesInputData-1); i++) {
        int8_t inputByte = (int8_t)*inputData++;
        AudioSampleType outputSample = (AudioSampleType)(inputByte * upScale);
        *outputSamples++ = outputSample;
//        printf("8BitScaled: decoder outputSample Value: %0.3f\r\n", (float)outputSample);
        (*outNumOutputSamples)++; // Inc output samples written
    }
}
    
// Utility functions to derive buffer requirements.

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitScaled<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    // This is simply 1 byte more than the number of samples, due to the scale value at the beginning.
    return (numSamples + 1);
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitScaled<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    // This is simple. It's just the number of samples multiplied by the audio output sample size.
    return sizeof(AudioSampleType) * numSamples;
}


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecCompressor_8BitScaled<AudioF32>;
template class AudioCodecCompressor_8BitScaled<AudioF64>;
template class AudioCodecCompressor_8BitScaled<AudioS8>;
template class AudioCodecCompressor_8BitScaled<AudioS16>;
template class AudioCodecCompressor_8BitScaled<AudioS32>;
