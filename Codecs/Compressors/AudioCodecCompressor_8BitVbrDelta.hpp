#include "../AudioCodec.hpp"
#include "AudioCodecCompressor_8BitScaled.hpp"


template <typename AudioSampleType> 
class AudioCodecCompressor_8BitVbrDelta : public AudioCodecCompressor_8BitScaled<AudioSampleType> {
private:
    // Buffers for compression
    AudioBytes *m_8BitScaledBuffer = NULL;  // For initial compression to 8-bit scaled format
    uint32_t    m_num8BitScaledBufferBytes = 0;
    int8_t     *m_8BitDeltaBuffer = NULL;  // For storing the 8-bit inter-sample delta values
    uint8_t    *m_deltaLengthBuffer = NULL;  // For storing the minimum bit lengths of each delta
    uint8_t    *m_codeBuffer = NULL;  // For storing the provisional codes for each delta

    // Buffer for decompression - we just need one for getting back to 8-bit samples
    // that we can use to send to the 8BitScaled decompressor.
    AudioBytes *m_8BitScaledDecompBuffer = NULL;  // For decompression of VbrDelta to 8-bit scaled format
    uint32_t    m_num8BitScaledDecompBufferBytes = 0;

    // Minimum run length where it is cost-effective to replace the current bit length with the 
    // one that is 1-bit smaller. (For silent runs, codes 0 & 1, this just stores the run length of those codes)
    // So, for a code of 3 (4-bit deltas), it is only worth inserting a run of code 2 (3-bit deltas) if that run
    // is at least 16 deltas long. So this is indexed by the code we could change to, to see if it's worth doing.
    static constexpr uint8_t minRun[7] = {4, 16, 13, 15, 17, 19, 21};

public:

    // For histograms of deltas
    int8_t m_deltaHistogram[256];

    // Compress a buffer of native samples.
    void compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) override;
    // Decompress a buffer of compressed samples. 
    // Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
    void decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    
    // Utility functions to derive buffer requirements.
    uint32_t getBytesMaxCompressedSize(uint32_t numSamples) override;
    uint32_t getBytesMaxDecompressedSize(uint32_t numSamples) override;

    ~AudioCodecCompressor_8BitVbrDelta() override {
        if (m_8BitScaledBuffer != NULL) free(m_8BitScaledBuffer);
        if (m_8BitDeltaBuffer != NULL) free(m_8BitDeltaBuffer);
        if (m_8BitScaledDecompBuffer != NULL) free(m_8BitScaledDecompBuffer);
        if (m_deltaLengthBuffer != NULL) free(m_deltaLengthBuffer);
        if (m_codeBuffer != NULL) free(m_codeBuffer);
    }
};