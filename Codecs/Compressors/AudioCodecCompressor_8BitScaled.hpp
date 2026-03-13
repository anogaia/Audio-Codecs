#ifndef __AUDIOCODECCOMPRESSOR_8BITSCALED_HPP__
#define __AUDIOCODECCOMPRESSOR_8BITSCALED_HPP__ 1

#include "../AudioCodec.hpp"


template <typename AudioSampleType> 
class AudioCodecCompressor_8BitScaled : public AudioCodecCompressor<AudioSampleType> {
public:
    // Compress a buffer of native samples.
    void compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) override;
    // Decompress a buffer of compressed samples. 
    // Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
    void decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    
    // Utility functions to derive buffer requirements.
    uint32_t getBytesMaxCompressedSize(uint32_t numSamples) override;
    uint32_t getBytesMaxDecompressedSize(uint32_t numSamples) override;
};


#endif // __AUDIOCODECCOMPRESSOR_8BITSCALED_HPP__