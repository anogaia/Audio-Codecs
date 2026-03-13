#ifndef __AUDIOCODEC_HPP__
#define __AUDIOCODEC_HPP__ 1

#include "Utility/AudioCodecUtils.hpp"

// Audio Sample Types
typedef float   AudioF32;
typedef double  AudioF64;
typedef int8_t  AudioS8;
typedef int16_t AudioS16;
typedef int32_t AudioS32;

// Compressed Audio Data that is not a transparent audio sample stream; just opaque bytes
typedef uint8_t AudioBytes;



// Filters for decimation and expansion
template <typename AudioSampleType> 
class AudioCodecFilter {
public:
    
    // Reset function - Useful when re-using an instance for a new audio stream, to prevent artefacts
    virtual void reset() {} // Clears all internal state

    // Worker functions to actually downsample or upsample audio data
    virtual void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples)=0;    
    virtual void expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples)=0;

    // Utility functions for callers to determine required buffer sizes for a particular input size
    virtual uint32_t getDecimatedSamples(uint32_t inputSamples)=0;
    virtual uint32_t getExpandedSamples(uint32_t inputSamples)=0;

     // Default Destructor - For proper polymorphism cleanup
    virtual ~AudioCodecFilter() = default;
};

// Basic implementation of a pass-through filter
template <typename AudioSampleType> 
class AudioCodecFilter_PassThrough : public AudioCodecFilter<AudioSampleType> {
public:
    void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;    
    void expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    uint32_t getDecimatedSamples(uint32_t inputSamples) override;
    uint32_t getExpandedSamples(uint32_t inputSamples) override;
};


// Compression and decompression functions for turning raw packets of samples into compressed audio data for transmission and back again.
template <typename AudioSampleType> 
class AudioCodecCompressor {
public:

    virtual void compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData)=0;
    virtual void decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples)=0;

    // Utility functions for callers to determine required buffer sizes
    // Call with the packet size in use, in terms of sample count.
    virtual uint32_t getBytesMaxCompressedSize(uint32_t numSamples)=0;
    virtual uint32_t getBytesMaxDecompressedSize(uint32_t numSamples)=0;

     // Default Destructor - For proper polymorphism cleanup
    virtual ~AudioCodecCompressor() = default;
};

// Basic implementation of pass-through compressor and decompressor, 1:1 or 0% compression ratio
template <typename AudioSampleType> 
class AudioCodecCompressor_PassThrough : public AudioCodecCompressor<AudioSampleType> {
public:
    void compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) override;
    void decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    uint32_t getBytesMaxCompressedSize(uint32_t numSamples) override;
    uint32_t getBytesMaxDecompressedSize(uint32_t numSamples) override;
};


// Audio Codec Squelcher pure virtual base
template <typename AudioSampleType> 
class AudioCodecSquelcher {
public:

    // Derived classes must provide an implementation for squelch
    virtual void squelch(AudioSampleType *inputSamples, uint32_t numInputSamples)=0;    

     // Default Destructor - For proper polymorphism cleanup
    virtual ~AudioCodecSquelcher() = default;
};

// AudioCodecSquelcher_PassThrough - A basic implementation of a squelcher that has no effect
template <typename AudioSampleType> 
class AudioCodecSquelcher_PassThrough : public AudioCodecSquelcher<AudioSampleType> {
public:
    void squelch(AudioSampleType *inputSamples, uint32_t numInputSamples) override {};
};





// This is a flexible base class that can be used for any codec.
// The basic functions are simply encoding and decoding.
// Filters used for filtering, decimation and expansion are pluggable.
// Codecs used for compression and decompression are pluggable.
// Squelchers used to clamp quiet stretches to zero, in order to improve compression, are also pluggable.
// Use the setFilter(), setCompressor() etc. setters to change these.
template <typename AudioSampleType> 
class AudioCodec {
private:

    // Default accessories - filter, compressor & squelcher
    AudioCodecFilter_PassThrough<AudioSampleType> defaultFilter;
    AudioCodecCompressor_PassThrough<AudioSampleType> defaultCompressor;
    AudioCodecSquelcher_PassThrough<AudioSampleType> defaultSquelcher;

    // Filter / Decimator object
    AudioCodecFilter<AudioSampleType> *m_filter = &defaultFilter;
    // Compressor object
    AudioCodecCompressor<AudioSampleType> *m_compressor = &defaultCompressor;
    // Squelcher object
    AudioCodecSquelcher<AudioSampleType> *m_squelcher = &defaultSquelcher;

    AudioSampleType *m_encodeBuffer = NULL;  // For encode, this is used to filter into, prior to compression.
    uint32_t m_encodeBufferSize = 0;
    AudioSampleType *m_decodeBuffer = NULL;  // For decode, this is used to decompress into, prior to expanding.
    uint32_t m_decodeBufferSize = 0;
    // For both encode and decode, the final output always goes straight to the (caller-supplied) output buffer.
    // Why do we have two of these, when they could share a single buffer? Multithreading.
    // It's entirely possible for two different threads to be doing encoding and decoding at the same time,
    // using the same codec object.
    // In fact, I should probably make both encode and decode functions block re-entry for that exact reason.
    // Even though they should only ever be called sequentially and be tied to a single audio stream each.

public:

    // Default Constructor
    AudioCodec() {};

    // Default Destructor
    virtual ~AudioCodec() {
        if (m_encodeBuffer != NULL) free(m_encodeBuffer);
        if (m_decodeBuffer != NULL) free(m_decodeBuffer);
    };


    virtual void setFilter(AudioCodecFilter<AudioSampleType> &newFilter) { m_filter = &newFilter; }
    virtual void setCompressor(AudioCodecCompressor<AudioSampleType> &newCompressor) { m_compressor = &newCompressor; }
    virtual void setSquelcher(AudioCodecSquelcher<AudioSampleType> &newSquelcher) { m_squelcher = &newSquelcher; }

    // Convenience function for consumers - get the size they need for the encoded output buffer.
    // I'm not making this virtual, because it needs to have standardised behaviour.
    // The output only depends on the currently assigned filter and compressor, so being able to mess with it 
    // would only add problems, not flexibility. 
    uint32_t getMaxEncodedBytes(uint32_t numInputSamples);

    // On entry, *outNumEncodedSamples points to a variable initialised with the maximum size of the output buffer that may be used.
    // On exit, the method updates this value to the actual size that was written.
    virtual void encode(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *encodedBytes, uint32_t *outNumEncodedBytes);

    // Decode works much the same as encode, in terms of output buffer usage and updating of the output buffer sample count that was written to.
    // The only difference is that this is in samples, not bytes.
    virtual void decode(AudioBytes *encodedBytes, uint32_t numEncodedBytes, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples);
};


#endif // __AUDIOCODEC_HPP__