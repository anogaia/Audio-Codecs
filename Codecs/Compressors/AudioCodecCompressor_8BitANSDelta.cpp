#include "AudioCodecCompressor_8BitANSDelta.hpp"

// 8-Bit ANS Audio Compressor
// Uses rANS to compress 8-bit deltas. Builds on top of the 8-bit scaled compressor.
// The 8-bit audio is downsampled from the audio input type by first converting to 16-bit PCM.
// This is then scaled down to 8-bit and the unsigned 8-bit scaling value is stored in the compressed audio packet
// as the first byte.
// The second byte is the first, uncompressed signed 8-bit audio sample.
// Subsequent sample deltas are all compressed using the rANS delta coding scheme.

// Implementation:
// This compressor derives directly and literally from the 8BitScaled compressor.
// So in this one, we first call the compress or decompress method of that class and then
// go on to further compress the data using the lossless variable bit length delta coding scheme.
// This requires managing an intermediate buffer for the raw 8-bit scaled encoding.
// I think it is correct for this class to manage this buffer, since it is rather specific to this
// particular encoding scheme and therefore belongs to the particular compressor rather than to the
// codec in general.

// Compress a buffer of native samples.
template <class AudioSampleType>
void AudioCodecCompressor_8BitANSDelta<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {

    // Ask 8BitScaled compressor how big a buffer we'll need.
    // Allocate a new one if the current one isn't big enough.
    uint32_t num8BitScaledBytes = AudioCodecCompressor_8BitScaled<AudioSampleType>::getBytesMaxCompressedSize(numInputSamples);
    if (num8BitScaledBytes > m_num8BitScaledBufferBytes) {
        // Allocate a new 8-bit scaled buffer.
        if (m_8BitScaledBuffer != NULL) free(m_8BitScaledBuffer);
        m_8BitScaledBuffer = (AudioBytes*)malloc(num8BitScaledBytes);
        m_num8BitScaledBufferBytes = num8BitScaledBytes;
        // Allocate a new 8-bit delta buffer.
        if (m_8BitDeltaBuffer != NULL) free(m_8BitDeltaBuffer);
        m_8BitDeltaBuffer = (int8_t*)malloc(num8BitScaledBytes-1);
        m_num8BitDeltaBufferBytes = num8BitScaledBytes-1;
    }

    // Get the 8BitScaled compressor to do the first stage and compress to the 8-bit scaled representation
    uint32_t outBytes8BitScaledOutputData = num8BitScaledBytes; 
    AudioCodecCompressor_8BitScaled<AudioSampleType>::compress(inputSamples, numInputSamples, m_8BitScaledBuffer, &outBytes8BitScaledOutputData);

    // Use the 8-bit scaled samples to create our 8-bit delta samples ready for variable bit length delta compression.
    uint32_t numDeltasToEncode = numInputSamples-1;
    for (int i=0; i<numDeltasToEncode; i++) {
        int16_t thisSample = (int8_t)(m_8BitScaledBuffer[i+1]);
        int16_t nextSample = (int8_t)(m_8BitScaledBuffer[i+2]);
        int16_t delta = nextSample - thisSample;
        int8_t delta8bit = (int8_t)delta;
        int8_t reconNextSample = (int8_t)thisSample + delta8bit;
//        printf("Delta[%d]: %d -> %d = %d as int8 or %d as int16. Reconstructed next sample: %d\n", i, thisSample, nextSample, delta8bit, delta, reconNextSample);
        m_8BitDeltaBuffer[i] = delta8bit;
    }

    // Histogram
    memset(m_deltaHistogram, 0, 256);   // Clear all entries to 0 to start with
    for (int i=0; i<numInputSamples-1; i++) {
        int8_t delta = m_8BitDeltaBuffer[i];
        m_deltaHistogram[(uint8_t)delta]++;       
    }

    // Zero the output data bytes count
    *outNumBytesOutputData = 0;

    // Write out the scaling value and first raw sample
    *outputBytes++ = m_8BitScaledBuffer[0]; // Scale value
    *outputBytes++ = m_8BitScaledBuffer[1]; // First raw sample
    // Increment output bytes count
    (*outNumBytesOutputData) += 2;

    // First, we need to map all of the signed delta values to unsigned ones,
    // so that we can preserve the magnitude ordering.
    // We'll append these to the vector we'll use for rANS compression, to save steps.
    std::vector<uint8_t> input_symbols;
    for (int i=0; i<numDeltasToEncode; i++) {
        int8_t delta = m_8BitDeltaBuffer[i];
        uint8_t mapped_delta = AudioCodecUtils::diag_map(delta);
        input_symbols.push_back(mapped_delta);
//        printf("Sample[%d] scaled = %d, delta = %d, mapped = %d\n", i, (int8_t)m_8BitScaledBuffer[i+2], delta, mapped_delta);
    }

    // Now we compress the mapped delta samples as input symbols to the rANS compressor
    AdaptiveFreqTable freq_table;
    rANSEncoder encoder;
    std::vector<uint8_t> compressed;

    // Encode symbols in reverse order (rANS encodes backwards)
    for (int i=0; i<numDeltasToEncode; i++) {
        uint8_t sym = input_symbols[i];
        uint16_t start = freq_table.cum_freq[sym];
        uint16_t freq = freq_table.freq[sym];
        encoder.encode(start, freq, compressed);
//        printf("Encoding rANS Data[%d] = %d (delta: %d)\n", i, sym, diag_unmap(sym));
        //freq_table.update(sym);
    }
    // Push one extra symbol
    uint8_t sym = input_symbols[0];
    uint16_t start = freq_table.cum_freq[sym];
    uint16_t freq = freq_table.freq[sym];
    encoder.encode(start, freq, compressed);
    encoder.flush(compressed);

    printf("rANS Compressed data length: %lu\n", compressed.size());

    // Write out the compressed data
    for (int i=0; i<compressed.size(); i++) {
        *outputBytes++ = compressed[i];
    }
    (*outNumBytesOutputData) += compressed.size();
}

// Decompress a buffer of compressed samples to native samples. 
// Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
template <class AudioSampleType>
void AudioCodecCompressor_8BitANSDelta<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // Ask 8BitScaled compressor how big a buffer we'll need.
    // Allocate a new one if the current one isn't big enough.
    uint32_t numOutputSamples = *outNumOutputSamples;
    uint32_t num8BitScaledBytes = AudioCodecCompressor_8BitScaled<AudioSampleType>::getBytesMaxCompressedSize(numOutputSamples);
    if (num8BitScaledBytes > m_num8BitScaledDecompBufferBytes) {
        // Allocate a new 8-bit scaled buffer.
        if (m_8BitScaledDecompBuffer != NULL) free(m_8BitScaledDecompBuffer);
        m_8BitScaledDecompBuffer = (AudioBytes*)malloc(num8BitScaledBytes);
        m_num8BitScaledDecompBufferBytes = num8BitScaledBytes;
    }

    int8_t m_8BitDeltaDecompBuffer[1024]; 

    // First, copy over the first 2 bytes to our internal buffer - 
    // the scale value and the initial sample value.
    m_8BitScaledDecompBuffer[0] = inputData[0]; // Scale value
    m_8BitScaledDecompBuffer[1] = inputData[1]; // Initial sample
    // Everything after this in the input data is compressed delta samples. 

    // Make a list of the rANS data from the input array
    std::vector<uint8_t> compressed;
    for (int i=0; i<numBytesInputData-2; i++) {
        compressed.push_back(inputData[i+2]);
//        printf("rANS Data[%d] = %d\n", i, inputData[i]);
    }

    // Decode rANS data
    AdaptiveFreqTable decode_freq_table;
    rANSDecoder decoder(compressed);
    std::vector<uint8_t> decoded_symbols;

    for (int i = numOutputSamples-1; i>=0; i--) {
        uint8_t sym = decoder.decode(compressed, decode_freq_table);
        decoded_symbols.push_back(sym);
//        printf("Decoding rANS Data[%d] = %d (delta: %d)\n", i, sym, diag_unmap(sym));
        //decode_freq_table.update(sym);
    }

    // Reverse decoded symbols to original order
//    std::reverse(decoded_symbols.begin(), decoded_symbols.end());

    // Copy the decoded mapped deltas into our intermediate buffer, 
    // unmapping them and converting from deltas to raw samples as we go
    int8_t current_sample = inputData[1];
    for (int i=0; i<numOutputSamples-1; i++) {
        uint8_t mapped_delta = decoded_symbols[i];
        int8_t delta = AudioCodecUtils::diag_unmap(mapped_delta);
        m_8BitDeltaDecompBuffer[i] = delta;
        current_sample += delta;
        m_8BitScaledDecompBuffer[i+2] = current_sample;
//        printf("rANS decoded data: mapped_delta[%d] = %d, delta[%d] = %d\n", i, mapped_delta, i, delta);
    }
    
    u_int32_t numIncorrectDecodes = 0;
    for (int i=0; i<numOutputSamples-1; i++) {
        int8_t inputValue = m_8BitScaledBuffer[i+1];
        int8_t outputValue = m_8BitScaledDecompBuffer[i+1];
        int8_t inputDelta = m_8BitDeltaBuffer[i];
        int8_t outputDelta = m_8BitDeltaDecompBuffer[i];
        if (inputValue != outputValue) numIncorrectDecodes++;
        printf("Decoding Check: Input[% 3d] = % 4d  Output[% 3d] = % 4d   InDelta[% 3d] = % 4d  OutDelta[% 3d] = % 4d\n", i, inputValue, i, outputValue, i, inputDelta, i, outputDelta);
    }
    printf("Incorrect Decoded Values: %d (%d %% of %d)\n", numIncorrectDecodes, (uint32_t)(100.0f*(float)numIncorrectDecodes/numOutputSamples), numOutputSamples);

    // Ok, we now have a buffer of scaled 8-bit samples. 
    // We can now pass this to the 8BitScaled decompressor
    AudioCodecCompressor_8BitScaled<AudioSampleType>::decompress(m_8BitScaledDecompBuffer, numOutputSamples+1, outputSamples, outNumOutputSamples);
}
    
// Utility functions to derive buffer requirements.

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitANSDelta<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    // I'm pretty new to arithmetic coding, so have to make a guess here.
    // I don't think there's much chance it will fail to compress at all, but let's give it 50%
    // headroom just in case.
    return ((numSamples+1) * 150) / 100;
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitANSDelta<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    // This is much simpler. It's just the number of samples multiplied by the audio output sample size.
    return sizeof(AudioSampleType) * numSamples;
}


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecCompressor_8BitANSDelta<AudioF32>;
template class AudioCodecCompressor_8BitANSDelta<AudioF64>;
template class AudioCodecCompressor_8BitANSDelta<AudioS8>;
template class AudioCodecCompressor_8BitANSDelta<AudioS16>;
template class AudioCodecCompressor_8BitANSDelta<AudioS32>;
