#include "../AudioCodec.hpp"
#include "AudioCodecCompressor_12BitScaled.hpp"


template <typename AudioSampleType>
class AudioCodecCompressor_12BitVbrDelta : public AudioCodecCompressor_12BitScaled<AudioSampleType> {
private:
    // Buffers for compression
    AudioBytes *m_12BitScaledBuffer = NULL;  // For initial compression to 12-bit scaled format
    uint32_t    m_num12BitScaledBufferBytes = 0;
    int16_t    *m_12BitSampleBuffer = NULL;  // Parsed 12-bit samples (int16 storage)
    int16_t    *m_12BitDeltaBuffer = NULL;   // Inter-sample deltas
    uint8_t    *m_deltaLengthBuffer = NULL;  // Minimum bit lengths of each delta
    uint8_t    *m_codeBuffer = NULL;         // Provisional codes for each delta
    uint32_t    m_numDeltaSlots = 0;

    // Buffer for decompression - rebuild 12-bit scaled bytes for the 12BitScaled decompressor.
    AudioBytes *m_12BitScaledDecompBuffer = NULL;
    uint32_t    m_num12BitScaledDecompBufferBytes = 0;

    // Minimum run length where it is cost-effective to replace the current bit length with the
    // one that is 1-bit smaller. (For silent runs, codes 0 & 1, this just stores the run length.)
    // Indexed by the code we could change to. Uses 4-bit length codes (codeBits=4), so
    // break-even is roughly R > 2*N + 7 for switching from N-bit to (N-1)-bit deltas.
    // Codes: 0/1 = silence, 2..11 = 3..12 bit deltas (deltaBits = code + 1).
    static constexpr uint8_t minRun[12] = {4, 16, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33};

    static constexpr uint8_t kCodeBits = 4;
    static constexpr uint8_t kMaxDeltaCode = 11; // 12-bit deltas

public:

    // Compress a buffer of native samples.
    void compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) override;
    // Decompress a buffer of compressed samples.
    // Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
    void decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;

    // Utility functions to derive buffer requirements.
    uint32_t getBytesMaxCompressedSize(uint32_t numSamples) override;
    uint32_t getBytesMaxDecompressedSize(uint32_t numSamples) override;

    ~AudioCodecCompressor_12BitVbrDelta() override {
        if (m_12BitScaledBuffer != NULL) free(m_12BitScaledBuffer);
        if (m_12BitSampleBuffer != NULL) free(m_12BitSampleBuffer);
        if (m_12BitDeltaBuffer != NULL) free(m_12BitDeltaBuffer);
        if (m_12BitScaledDecompBuffer != NULL) free(m_12BitScaledDecompBuffer);
        if (m_deltaLengthBuffer != NULL) free(m_deltaLengthBuffer);
        if (m_codeBuffer != NULL) free(m_codeBuffer);
    }
};
