#include "AudioCodec.hpp"


// Core code, common to all codecs.
// The magic happens in the filters and compressors, really.
// This code just calls through to the filter and compressor assigned to actually do the work.
// In case you need more flexibility, the encode and decode functions are virtual, so can be overridden in derived classes.
template <class AudioSampleType>
void AudioCodec<AudioSampleType>::encode(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *encodedBytes, uint32_t *outNumEncodedBytes) {

    // First, we'll squelch the input audio using the current squelch function
    m_squelcher->squelch(inputSamples, numInputSamples);

    // First, we may need to allocate a new buffer to filter into.
    uint32_t numDecimatedSamples = m_filter->getDecimatedSamples(numInputSamples);
    uint32_t filterBufferSize = numDecimatedSamples * sizeof(AudioSampleType);
    if (m_encodeBufferSize < filterBufferSize) {
        if (m_encodeBuffer != NULL) free(m_encodeBuffer);
        m_encodeBuffer = (AudioSampleType*)malloc(filterBufferSize);
        m_encodeBufferSize = filterBufferSize;
    }

    // Ok, we have our buffer. Now we can filter into it.
    uint32_t numSamplesWritten = numDecimatedSamples;
    m_filter->decimate(inputSamples, numInputSamples, m_encodeBuffer, &numSamplesWritten);
    // In theory, that should have written exactly the number of output samples we expected it to.
    // So, numSamplesWritten should now be equal to numDecimatedSamples.

    // Now we have filtered and decimated our input audio, we can compress it. 
    // This goes straight to the supplied output buffer.
    m_compressor->compress(m_encodeBuffer, numDecimatedSamples, encodedBytes, outNumEncodedBytes);
    // We just pass through the out parameter here. The compressor shouldn't ever go out of bounds on that output buffer,
    // but if it did, well, it's too late for this function to do anything about that anyway.

    // Possibly, we should at least check this for sanity and maybe return a bool or error code to signal how it went.
}

template <class AudioSampleType>
void AudioCodec<AudioSampleType>::decode(AudioBytes *encodedBytes, uint32_t numEncodedBytes, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // First, we may need to allocate a new buffer to decompress into.
    // Working out how big this should be isn't as simple as for encoding.
    // All we can really do is hope that the caller has set enough memory aside for the final output.
    // We need less for the decompression buffer, since that occurs before it gets expanded.
    uint32_t numDecimatedSamples = m_filter->getDecimatedSamples(*outNumOutputSamples);
    uint32_t decompressionBufferSize = numDecimatedSamples * sizeof(AudioSampleType);
    if (m_decodeBufferSize < decompressionBufferSize) {
        if (m_decodeBuffer != NULL) free(m_decodeBuffer);
        m_decodeBuffer = (AudioSampleType*)malloc(decompressionBufferSize);
        m_decodeBufferSize = decompressionBufferSize;
    }

    // Ok, we have our buffer. Now we can decompress into it.
    uint32_t numSamplesWritten = numDecimatedSamples;
    printf("Codec::decode() decoding %d samples.\n", numSamplesWritten);
    m_compressor->decompress(encodedBytes, numEncodedBytes, m_decodeBuffer, &numSamplesWritten);
    // In theory, that should have written exactly the number of output samples we expected it to.
    // So, numSamplesWritten should now be equal to numDecimatedSamples.
    // Of course, we have the caveat that the caller could be using a much bigger buffer than they need, so
    // this isn't guaranteed to be accurate. Still, it's very likely that they know how many samples their data 
    // should decompress into and have been economical enough to size their buffer accordingly.

    // Now we have decompressed our compressed audio, we can expand it back to full sample rate.
    m_filter->expand(m_decodeBuffer, numDecimatedSamples, outputSamples, outNumOutputSamples);
    // We just pass through the out parameter here. The expander shouldn't ever go out of bounds on that output buffer,
    // but if it did, well, it's too late for this function to do anything about that anyway.

    // Possibly, we should at least check this for sanity and maybe return a bool or error code to signal how it went.
}


template <class AudioSampleType>
uint32_t AudioCodec<AudioSampleType>::getMaxEncodedBytes(uint32_t numInputSamples) {
    uint32_t decimatedSamples = m_filter->getDecimatedSamples(numInputSamples);
    uint32_t compressedSize = m_compressor->getBytesMaxCompressedSize(decimatedSamples);
    return compressedSize;
}


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodec<AudioF32>;
template class AudioCodec<AudioF64>;
template class AudioCodec<AudioS8>;
template class AudioCodec<AudioS16>;
template class AudioCodec<AudioS32>;





// Built-In Default Filter & Compressor

// Basic implementation of a pass-through filter; just copies samples
template <class AudioSampleType> 
void AudioCodecFilter_PassThrough<AudioSampleType>::decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples);
    memcpy(outputSamples, inputSamples, numInputSamples * sizeof(AudioSampleType));       
    *outNumOutputSamples = numInputSamples;
}
    
template <class AudioSampleType>
void AudioCodecFilter_PassThrough<AudioSampleType>::expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples);
    memcpy(outputSamples, inputSamples, numInputSamples * sizeof(AudioSampleType));       
    *outNumOutputSamples = numInputSamples;
}    

template <class AudioSampleType>
uint32_t AudioCodecFilter_PassThrough<AudioSampleType>::getDecimatedSamples(uint32_t inputSamples) {
    return inputSamples; 
}

template <class AudioSampleType>
uint32_t AudioCodecFilter_PassThrough<AudioSampleType>::getExpandedSamples(uint32_t inputSamples) {
    return inputSamples; 
}



// Basic implementation of pass-through compressor and decompressor, 1:1 or 0% compression ratio
template <class AudioSampleType>
void AudioCodecCompressor_PassThrough<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {
    uint32_t inputDataSize = numInputSamples * sizeof(AudioSampleType);
    *outNumBytesOutputData = std::min(*outNumBytesOutputData, inputDataSize);
    memcpy(outputBytes, inputSamples, *outNumBytesOutputData);
}

template <class AudioSampleType>
void AudioCodecCompressor_PassThrough<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {
    uint32_t outputDataSize = *outNumOutputSamples * sizeof(AudioSampleType);
    numBytesInputData = std::min(numBytesInputData, outputDataSize);
    memcpy(outputSamples, inputData, numBytesInputData);
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_PassThrough<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    return numSamples * sizeof(AudioSampleType); 
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_PassThrough<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    return numSamples * sizeof(AudioSampleType); 
}

