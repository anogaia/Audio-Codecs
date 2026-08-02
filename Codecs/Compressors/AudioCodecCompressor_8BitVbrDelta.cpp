#include "AudioCodecCompressor_8BitVbrDelta.hpp"

// 8-Bit VBR Audio Compressor
// Uses variable bit-length (equivalent to VBR) delta coding to compress an 8-bit audio stream.
// The 8-bit audio is downsampled from the audio input type by first converting to 16-bit PCM.
// This is then scaled down to 8-bit and the unsigned 8-bit scaling value is stored in the compressed audio packet
// as the first byte.
// The second byte is the first, uncompressed signed 8-bit audio sample.
// Subsequent samples are all compressed using this delta coding scheme:
//
// The first 3 bits are a delta size indicator and also have a couple of values for different lengths of silence.
// 0 = 4 silent samples
// 1 = 16 silent samples
// 2 = 3-bit delta samples (-3 to +3) follow
// 3 = 4-bit delta samples (-7 to +7) follow
// 4 = 5-bit delta samples (-15 to +15) follow
// 5 = 6-bit delta samples (-31 to +31) follow
// 6 = 7-bit delta samples (-63 to +63) follow
// 7 = 8-bit delta samples (-127 to +127) follow

// This delta size indicator is followed by a run (any length) of delta samples of the indicated size.
// Each delta sample is coded as the difference between the previous sample and the current one.
// There are no scaling or logarithmic look-up values. All values are used directly, as-is.

// The unsymmetric most-negative value of each delta size (-4 for 3-bit, -8 for 4-bit etc.) is used to
// indicate an end of run.

// Therefore, after an end of run indicator, the next 3 bits are always a new delta size indicator and
// begin a new run at the new delta sample size.

// The 0 and 1 delta size indicator values are special and are not followed by delta samples, since they encode only silence.
// These have an intrinsic length in terms of how many samples they expand to and so do not require
// an end of run indicator.
// They are always followed by another 3-bit delta size indicator value.
// Their length may overrun the number of output samples if they occur at the end of an audio packet.
// The original packet length (or audio sample buffer length) should take precedence in that case.

// NOTE: For 8-bit samples, it is possible for a delta sample to cause the signed 8-bit accumulator to wrap
//       around. This should be ignored. It should be very rare that a single delta needs to step further
//       than +/-127, but it is possible. The encoder will do this if needed, to faithfully reproduce the
//       input signal.

// Implementation:
// This compressor derives directly and literally from the 8BitScaled compressor.
// So in this one, we first call the compress or decompress method of that class and then
// go on to further compress the data using the lossless variable bit length delta coding scheme.
// This requires managing an intermediate buffer for the raw 8-bit scaled encoding.
// I think it is correct for this class to manage this buffer, since it is rather specific to this
// particular encoding scheme and therefore belongs to the particular compressor rather than to the
// codec in general.

// Performance Notes:
// When testing this, the above NOTE describes a situation I didn't adequately plan for.
// The fact that high amplitude, high frequency signals (near the nyquist limit for the sample rate)
// could produce deltas that needed to wrap around in order to reproduce the large changes in the input
// samples, means that it could indeed go outside of the +/-127 range.
// This is a problem, because -128 is the end-of-run symbol for 8-bit deltas.
// Therefore, if left unchecked, this could produce decoding errors.
//
// There are 2 possible solutions:
//
// 1. Adjust the 8BitScaled compressor so that it produces effectively 7-bit signed values rather than 8-bit.
//    By restricting the sample value range to +/-63, the maximum size of the inter-sample deltas is reduced
//    to +/-126, so we never have any need to encode strictly out-of-range delta values, meaning we don't 
//    ever encounter the problem of trying to encode a delta of -128.
//
// 2. Keep the full 8-bit scale of input samples, but treat any -128 deltas differently.
//    The simplest scheme is to adjust any -128 deltas to -127 and then to feed the +1 offset error forward to
//    the next delta. The next sample will never be another -128 value, since that would push the reconstructed
//    next sample value out of range, so this is safe to do without worrying about the actual value of the next
//    delta. All we need to do is to subtract 1 from that delta.
//
// Advantages/disadvantages:
//
// Scheme 1 works in a stable fashion regardless of input and produces stable, constant distortion performance.
// Unfortunately, it does this at the cost of doubling the inherent distortion for all signals, due to dropping
// down from 8-bit to 7-bit raw samples.
//
// Scheme 2 preserves 8-bit raw samples, but introduces a tiny bit of distortion whenever a high energy signal
// causes large delta values to be encoded, which has a small chance of producing a delta of -128. 
// When it does happen, it only lasts for 2 samples, does not incur any lasting offset error, and by the very
// nature of high energy audio signals, this tiny and highly transient inaccuracy is very unlikely to be audible
// or to add more than an inconsequential amount of harmonic distortion.
//
// Conclusion: Scheme 2 has superior audio quality, maintains the stand-alone function of the 8BitScaled compressor 
//             without any modifications being required and does not impact the overall performance of the codec.
//
// First, I implemented Scheme 1, since that was a very easy fix. It does indeed give entirely error-free
// reconstruction, regardless of input signal. 
// In future, I think it is worth re-coding this to use Scheme 2, since while it does technically introduce
// a small transient error to a couple of samples when it occurs, the probability of that event is really very
// low and won't occur at all during most speech audio. Indeed, after the 4X filter is improved somewhat, it may
// not occur at all anymore, due to the better rolloff and lower level of high frequency signals near the nyquist
// limit.
//
// Implementation of Scheme 2:
//
// The best way seems to be to actually adjust the input samples before deltas are calculated, really.
// A less pure approach would be to do this during the 8BitScaled compression function, while scaling from the
// 16-bit sample representation.
// This, or an extra pass at the beginning of the 8BitVbrDelta compressor, would allow later comparison of
// the decoded samples, after delta reconstruction, with the original samples. Indeed, this allows the 
// checking of the compressor and decompressor to avoid treating the -128 issue as an error, since the signal
// has effectively been adjusted before encoding happened.
// An optimised way to do this could be to perform the adjustments during delta calculation but also rewrite
// the samples involved in the input buffer. A bit naughty, maybe, but then again, this will only ever be 
// done for debug versions where we're running the runtime checks. For release, we'd just not care about the 
// off-by-one sample errors introduced.
//
// Scheme 2 Implemented. Some notes on how that went:
// I've rolled back the changes to the 8BitScaled compressor. It now produces full-scale 8-bit samples again.
// The -128 adjustment scheme is implemented in the 8BitVbrDelta compressor, including the forward error
// propagation that corrects the next delta for the +1 added to the -128 delta to make it -127.
// I didn't realise at first, that this makes the next sample immediately correct after decoding from deltas.
// So the only error ends up being the -128 delta's sample, so just one sample is off by 1 during this 
// 'correction' scheme, when it happens. That's definitely not going to be audible.
// Overall, I'm happy to leave it here. The compressor seems to work very well, yielding a compression factor
// between 1.2 and 2.1, averaging around 1.4, compared to using 8-bit scaling alone. Definitely worth doing.
// I also realised that I had made a mistake when calculating the break-even point of inserting a smaller
// bit-length run. I had failed to take into account the extra end-of-run and delta length code required
// at the end of the inserted run. This makes the minimum length runs about twice as long as before.
// I've updated the minRun[] array in the header file to reflect the new, corrected values.
// I only noticed this when it was compressing packets that had mostly 8-bit deltas and it was inserting
// a few 7-bit runs that just pushed the compressed data length over the 258 bytes maximum that it should
// ever produce. After all, the whole point of this compressor is that it predicts the cost of switching
// to different delta lengths and only does so if it produces a smaller output encoding.
// It actually performs a lot better in general now, giving over 2.0 compression factor on many packets.
// After adding a gentle squelch function to the input audio, it gets over 1.5 on almost all test files.

// Prototypes for internal helper functions
void prepareDeltaAndCodeBuffers(uint32_t numDeltas, int8_t *samples, int8_t *deltas, uint8_t *deltaLengths, uint8_t *codes);
void prepareBitLengthChanges(uint32_t numDeltas, uint8_t *deltaLengths, uint8_t *codes, const uint8_t *minRun);
void prepareSilentRuns(uint32_t numDeltas, int8_t *samples, uint8_t *codes, const uint8_t *minRun);


// Compress a buffer of native samples.
template <class AudioSampleType>
void AudioCodecCompressor_8BitVbrDelta<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {

    // Ask the 8BitScaled compressor how big a buffer we'll need.
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
        // Allocate a new delta length buffer.
        if (m_deltaLengthBuffer != NULL) free(m_deltaLengthBuffer);
        m_deltaLengthBuffer = (uint8_t*)malloc(num8BitScaledBytes-1);
        // Allocate a new code buffer.
        if (m_codeBuffer != NULL) free(m_codeBuffer);
        m_codeBuffer = (uint8_t*)malloc(num8BitScaledBytes-1);
    }

    // Get the 8BitScaled compressor to do the first stage and compress to the 8-bit scaled representation
    uint32_t outBytes8BitScaledOutputData = num8BitScaledBytes; 
    AudioCodecCompressor_8BitScaled<AudioSampleType>::compress(inputSamples, numInputSamples, m_8BitScaledBuffer, &outBytes8BitScaledOutputData);

    // Calculate the number of inter-sample deltas we need to work with
    uint32_t numDeltasToEncode = numInputSamples-1;

    // Calculate deltas, their minimum bit widths for encoding and initialise the codes buffer.
    prepareDeltaAndCodeBuffers(numDeltasToEncode, (int8_t*)m_8BitScaledBuffer, m_8BitDeltaBuffer, m_deltaLengthBuffer, m_codeBuffer);

    // Go through the deltas and compress them to use the smallest bit widths possible, wherever that uses less bits.
    prepareBitLengthChanges(numDeltasToEncode, m_deltaLengthBuffer, m_codeBuffer, minRun);

    // Now we can check for silent runs. No need for cost vs run length - silent codes are always cheaper.
    prepareSilentRuns(numDeltasToEncode, (int8_t*)m_8BitScaledBuffer, m_codeBuffer, minRun);

    // Ok, now we're ready to actually encode this into a bit-packed stream.

    // Write out the scaling value and the first raw sample
    *outputBytes++ = m_8BitScaledBuffer[0]; // Scale value
    *outputBytes++ = m_8BitScaledBuffer[1]; // First raw sample

    uint32_t inputIndex = 0; // Index of the sample or delta being compressed
    uint32_t outputBitIndex = 0; // Track our output data bit position

    // Keep going until we have encoded all the input samples
//    printf("Starting to encode bit-packed delta and silent codes...\n");
    while (inputIndex < numDeltasToEncode) {

        uint8_t code = m_codeBuffer[inputIndex];
        uint8_t numSamplesEncoded = 0;  // Keep track of how many samples each cycle encodes, so we can skip ahead correctly

//        printf("Input Sample/Delta Index: %d\n", inputIndex);        
        if (code < 2) {
            // We have a silent run. Encode it and move on.
            AudioCodecUtils::write_nbits(outputBitIndex, outputBytes, 3, code);
            outputBitIndex += 3;
            numSamplesEncoded = minRun[code];
//            printf("Encoding a silent run of %d samples.\n", numSamplesEncoded);
        }
        else {
            // We haven't got a silent run, so we need to search ahead and try to compress deltas.
            // First, get the initial delta for this run.
            int8_t delta = m_8BitDeltaBuffer[inputIndex];
            // Calculate what delta length we need to encode this delta
            uint8_t deltaBits = code + 1;
//            printf("Starting to encode a run of delta samples. First delta: %d, numBits: %d\n", delta, deltaBits);
            // Now search ahead until we hit a different code
            uint32_t runLength = 1;
            for (int i=inputIndex+1; i<numDeltasToEncode; i++) {
                if (m_codeBuffer[i] == code) runLength++; else break;
            }
//            printf("Found a run of %d deltas that fit in %d bits. Encoding...\n", runLength, deltaBits);
            // Output the run to the bit-packed output buffer
            AudioCodecUtils::write_nbits(outputBitIndex, outputBytes, 3, code); // Write the delta run length code
            outputBitIndex += 3;
            for (int i=0; i<runLength; i++) {
                AudioCodecUtils::write_nbits(outputBitIndex, outputBytes, deltaBits, m_8BitDeltaBuffer[inputIndex+i]); // Write the delta
                outputBitIndex += deltaBits;
            }
            // Only output the end of run code if this is NOT the last run.
            if (inputIndex + runLength < numDeltasToEncode) {
                AudioCodecUtils::write_nbits(outputBitIndex, outputBytes, deltaBits, 1 << code); // Write the end of run code            
                outputBitIndex += deltaBits;
            } else {
//                printf("Not encoding end-of-run symbol, 'cos we're at the end.\n");
            }
            numSamplesEncoded = runLength;
        }
        inputIndex += numSamplesEncoded;
//        printf("Encoded %d samples/deltas so far. %d samples/deltas remaining to be encoded.\n", inputIndex, numDeltasToEncode - inputIndex);
    }

    // Calculate the total outputted bytes from the fixed bytes and the encoded bits.
    *outNumBytesOutputData = (outputBitIndex + 16 + 7) >> 3;
    printf("Finished encoding... Written %d bits of data. (%d bytes)\n", outputBitIndex + 16, *outNumBytesOutputData);
}



// Decompress a buffer of compressed samples to native samples. 
// Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
template <class AudioSampleType>
void AudioCodecCompressor_8BitVbrDelta<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

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

    // First, copy over the first 2 bytes to our internal buffer - 
    // the scale value and the initial sample value.
    m_8BitScaledDecompBuffer[0] = inputData[0];
    m_8BitScaledDecompBuffer[1] = inputData[1];
    // Everything after this in the input data is bit-packed. 
    uint32_t outputDataIndex = 2; // Start of decoded sample data.
    
    //Initialise state variables.
    bool modeIsDelta = false;   // Mode is either delta or code. VbrDelta data always starts with a code.
    uint8_t currentDeltaLength = 0; // When in delta mode, this is the current number of bits per delta sample.
    AudioBytes *bitPackedInputData = &(inputData[2]); // Start of bit-packed data.
    uint32_t inputDataBitIndex = 0; // Index into the input data in bits, for bit unpacking.
    uint8_t currentEndOfRunCode = 0; // The unsymmetric largest negative delta value for the current delta length.
    uint8_t silenceLengthRemaining = 0; // To keep track of how many silent samples we need to output.
    int8_t currentSample = (int8_t)inputData[1]; // To keep track of the current sample value as we add deltas to it.
    
    // Keep going until we fill the output buffer with the correct number of bytes.
    // This stops us from cases like having a 16 sample silence code near the end of a packet, which
    // could easily overrun the desired packet length. Instead, we just keep going until the packet is filled.
    while (outputDataIndex < num8BitScaledBytes) {
        if (modeIsDelta) {
            // We need to read the next value as a delta sample, which could also be an end-of-run code.
            uint8_t delta = AudioCodecUtils::read_nbits(inputDataBitIndex, bitPackedInputData, currentDeltaLength);
            inputDataBitIndex += currentDeltaLength;
            if (delta == currentEndOfRunCode) {
                // Current delta run ends here. Read a new code next.
                modeIsDelta = false;
            } else {
                // Update the current sample according to the delta value and write it into the buffer
                currentSample += AudioCodecUtils::sign_extend(delta, currentDeltaLength);
                m_8BitScaledDecompBuffer[outputDataIndex++] = currentSample;
            }
        } else {
            // Are we currently outputting a silence run?
            if (silenceLengthRemaining > 0) {
                m_8BitScaledDecompBuffer[outputDataIndex++] = 0;
                silenceLengthRemaining--;
            } else {
                // Read a 3-bit silence run or delta length code.
                uint8_t code = AudioCodecUtils::read_nbits(inputDataBitIndex, bitPackedInputData, 3);
                inputDataBitIndex += 3;
                if (code < 2) { // This is a silence run code.
                    currentSample = 0; // Make sure that after this, we start with a zero sample value for future deltas.
                    silenceLengthRemaining = minRun[code]; // Output a run of 4 or 16 zero samples.
                } else { // This is a delta length code.
                    currentDeltaLength = code + 1; currentEndOfRunCode = 1 << code;
                    modeIsDelta = true; // Switch to delta mode to read the delta samples ahead
                }
            }
        }
    }

    // Optional encode/decode self-check (only valid if this instance just compressed).
    if (m_8BitScaledBuffer != NULL && m_num8BitScaledBufferBytes >= numOutputSamples) {
        uint32_t numIncorrectDecodes = 0;
        uint32_t totalError = 0;
        for (uint32_t i = 0; i < numOutputSamples; i++) {
            int8_t inputValue = (int8_t)m_8BitScaledBuffer[i];
            int8_t outputValue = (int8_t)m_8BitScaledDecompBuffer[i];
            if (inputValue != outputValue) {
                numIncorrectDecodes++;
                totalError += abs(inputValue - outputValue);
            }
        }
        printf("Incorrect Decoded Values: %d (%0.1f %% of %d). Total error: %d\n", numIncorrectDecodes,
               100.0f * (float)numIncorrectDecodes / (float)numOutputSamples, numOutputSamples, totalError);
    }

    // Ok, we now have a buffer of scaled 8-bit samples.
    // We can now pass this to the 8BitScaled decompressor for reconstruction to the original sample type
    AudioCodecCompressor_8BitScaled<AudioSampleType>::decompress(m_8BitScaledDecompBuffer, outputDataIndex,
                                                                 outputSamples, outNumOutputSamples);
}
    

// Utility functions to derive buffer requirements.

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitVbrDelta<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    // Worst-case scenario is when we need to encode all deltas as 8-bit.
    // So it's 8 bits per delta encoded sample, plus scaling value plus initial sample plus 3 bits for the 8-bit delta size indicator.
    uint32_t bits = (numSamples-1) * 8 + 8 + 8 + 3;
    // Round up to the nearest byte
    return (bits + 7) >> 3;
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_8BitVbrDelta<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    // This is much simpler. It's just the number of samples multiplied by the audio output sample size.
    return sizeof(AudioSampleType) * numSamples;
}


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecCompressor_8BitVbrDelta<AudioF32>;
template class AudioCodecCompressor_8BitVbrDelta<AudioF64>;
template class AudioCodecCompressor_8BitVbrDelta<AudioS8>;
template class AudioCodecCompressor_8BitVbrDelta<AudioS16>;
template class AudioCodecCompressor_8BitVbrDelta<AudioS32>;



// Helper Function Definitions

// Use the 8-bit scaled samples to create our 8-bit delta samples ready for variable bit length delta compression.
// Also, precompute the minimum bit width we can store each delta in, so we don't have to keep recomputing it 
// during compression.
// Finally, initialise our code buffer as we go, to start with 8-bit deltas.
void prepareDeltaAndCodeBuffers(uint32_t numDeltas, int8_t *samples, int8_t *deltas, uint8_t *deltaLengths, uint8_t *codes) {
    
    int8_t errorCorrection = 0;
    for (int i=0; i<numDeltas; i++) {
        int16_t thisSample = (int8_t)(samples[i+1]);
        int16_t nextSample = (int8_t)(samples[i+2]);
        int16_t delta = nextSample - thisSample + errorCorrection;
        errorCorrection = 0;
        int8_t delta8bit = (int8_t)delta;
        int8_t reconNextSample = (int8_t)thisSample + delta8bit;
//        printf("Delta[%d]: %d -> %d = %d as int8 or %d as int16. Reconstructed next sample: %d\n", i, thisSample, nextSample, delta8bit, delta, reconNextSample);
        uint8_t minDeltaBits = AudioCodecUtils::count_significant_bits(delta8bit);
        // Avoid the unsymmetric most negative delta values - those are our end-of-run indicators.
//        if (delta8bit == (1 << (minDeltaBits-1))) delta8bit++;
        if (delta8bit == -128) {
            printf("************* Got a naughty -128 delta @ pos[%d]. Correcting to -127 and propagating error to next delta.\n", i);
            delta8bit = -127;   // This will introduce a slight offset error in the decoder, but it's minimal.
            // Still, not truly lossles anymore.
            // To correct this error, we can just subtract one from the next delta, so it becomes transient rather than
            // creating a permanent offset in all further samples.
            errorCorrection = -1;
        }
        // Store values in the arrays
        deltas[i] = delta8bit;
        deltaLengths[i] = minDeltaBits;
        // Initialise all entries in our provisional code buffer to 7, to reflect that we will
        // start with all deltas being encoded with the largest bit length.
        codes[i] = 7; 
    }
}

// Go through the delta buffer once for each delta bit length, checking if we can insert any
// runs of the next smallest code. There is no point going lower than 3 bits, since that's the last
// point where we actually have a smaller bit length we could insert.
void prepareBitLengthChanges(uint32_t numDeltas, uint8_t *deltaLengths, uint8_t *codes, const uint8_t *minRun) {
    for (int code=7; code>2; code--) {
        uint8_t currentDeltaLength = code + 1;
        uint32_t minRunLength = minRun[code-1]; // Minimum run length of the next smallest delta length, to be worth replacing
        uint32_t runStart = 0, runLength = 0;
        for (int i=0; i<numDeltas; i++) {
            uint8_t deltaLength = deltaLengths[i];
            if (runLength > 0) {
                // Already inside a potential run. Keep checking.
                if (deltaLength < currentDeltaLength) runLength++;
                else {
                    // We've ended that run - the current delta is not shorter.
                    // Is it worth it to insert a run at the smaller delta length?
                    if (runLength > minRunLength) {
                        // Set the codes for this new run with the next-smallest code
                        for (int u=runStart; u<(runStart+runLength); u++) { codes[u] = code-1; }
                    }
                    runLength = 0;
                }
            } else  {
                // Not inside a run. Check if we might start one.
                if (deltaLength < currentDeltaLength) { runStart = i; runLength++; }
            }
        }
        // We might have gone off the end of the delta buffer without marking the run we were on.
        // In this case, the minimum run length is a bit different - it's calculated without the 
        // cost of an end run marker. Technically, I should compute a separate table for these.
        // The difference is only a few bits though, so probably not very important.
        if (runLength > minRunLength) {
            // Set the codes for this new run with the next-smallest code
            for (int u=runStart; u<(runStart+runLength); u++) { codes[u] = code-1; }
        }
    }
}

// Update the code buffer with any places where we can get away with silent run codes.
void prepareSilentRuns(uint32_t numDeltas, int8_t *samples, uint8_t *codes, const uint8_t *minRun) {
    // Look for 16 sample ones first, then 4 samples ones.
    for (int code=1; code>=0; code--) {
        uint32_t minRunLength = minRun[code];
        uint32_t runStart = 0, runLength = 0;
        for (int i=0; i<numDeltas; i++) {
            // If we're doing 4 sample silent runs, we need to skip over
            // any 16 sample runs we've already laid down.
            if ((code == 0) && (codes[i] == 1)) i += 15;
            else {
                if (runLength > 0) {
                    // Already inside a potential run. Keep checking.
                    if (samples[i+2] == 0) runLength++;
                    else {
//                        printf("Silent Run (code %d, for %d samples) End @ position %d, length %d\n", code, minRunLength, i, runLength);
                        // We've ended that run - the current sample is not silent.
                        // Is it worth it to insert a silent run here?
                        if (runLength >= minRunLength) {
                            printf("**** Silent Run Found ****  Inserting a %d-long run of %d-sample silent codes.\n", runLength/minRunLength, minRunLength);
                            // Set the silent code for this run at the start and at every run length
                            // afterwards. No point setting the ones in-between - we'll skip over those.
                            for (int u=runStart; u<(runStart+runLength-(minRunLength-1)); u+=minRunLength) { codes[u] = code; }
                        }
                        runLength = 0;
                    }
                } else {
                    // Not inside a run. Check if we might start one.
                    if (samples[i+2] == 0) {
                         runStart = i; runLength++; 
                    //     printf("Silent Run (code %d, for %d samples) Start @ position %d\n", code, minRunLength, i); 
                    }
                }
            }
        }

    //    printf("Finished checking for silent runs of %d samples. RunStart=%d, RunLength=%d\n", minRunLength, runStart, runLength);
        // Again, we might have gone off the end of the delta buffer without marking the run we were on.
        // If so, fill in this last silent run.
        if (runLength > 0 && runLength == numDeltas - runStart) {
            // Set the silent code for this run at the start and at every run length
            // afterwards. No point setting the ones in-between - we'll skip over those.
            runLength = ((runLength + minRunLength-1) / minRunLength) * minRunLength;
            printf("**** Silent Run Found ****  Inserting a %d-long run of %d-sample silent codes.\n", runLength/minRunLength, minRunLength);
            for (int u=runStart; u<(runStart+runLength-1); u+=minRunLength) { codes[u] = code; }
        }
    }
}






/*
Diagnostic code from inside the compressor - Good to keep around, just in case I make a change and 
need a sanity check

    // Print out the codes so we can see what runs have been optimised and whether it looks sane...
    // We can also calculate how many bits this should take to encode.
    uint8_t currentCode = 99;
    uint32_t totalBits = 16; // We always start with a scale byte and an initial sample byte.
    uint8_t numCodeChanges = 0, numSilentRuns = 0;
    uint16_t silentSamples = 0, totalSilentSamples = 0;
    uint8_t code = 0, bits = 0;
    
    for (int i=0; i<numDeltasToEncode; i++) {
        code = m_codeBuffer[i];
        bits = 0;
        if (code < 2) {
            // Silent run.
            bits += 3;
            uint8_t silentRunLength = minRun[code]; 
            silentSamples = silentRunLength - 1;
            numSilentRuns++;
            // Unless we're at the very beginning, changing to a silent run also requires adding in the
            // end-of-run marker length for the previous code, unless it was also a silent code
            if (i > 0 && currentCode > 1) bits += currentCode + 1;
            currentCode = code;
            totalSilentSamples++;
        }
        else {
            // If we change code, then we need to add the end run marker and 3 bits for the new run code.
            if (silentSamples > 0) { silentSamples--; totalSilentSamples++; } // Just skip over codes inside silent runs
            else {
                if (code != currentCode) { 
                    // Unless we're at the very beginning, changing codes also requires adding in the
                    // end-of-run marker length for the previous code.
                    // Unless the previous code was a silent run of course, in which case it didn't have one.
                    if (i > 0 && currentCode > 1) bits += currentCode + 1;
                    // For the code change, we need to add in the switch code length itself
                    bits += 3; 
                    currentCode = code; numCodeChanges++;
                }
                bits += currentCode + 1; // Add the bits for this delta to be encoded
            }
        }
        totalBits += bits;

        char strDesc[32];
        if (code < 2) {
            sprintf(strDesc, "%s", code == 0 ? "Silent, 4 samples" : "Silent, 16 samples");
        } else {
            sprintf(strDesc, "%d bits", code+1);
        }
        printf("Sample[% 4d] = % 4d   Delta[% 4d] = % 4d   Code[% 4d] = %d (%s) -> [%d bits]\n", i, (int8_t)m_8BitScaledBuffer[i+2], i, m_8BitDeltaBuffer[i], i, code, strDesc, bits);

    }
    uint32_t totalBytes = (totalBits + 7) >> 3;
    printf("Estimated packet size with these coded runs: %d bits (%d bytes).\n(%d bit length changes / %d silent codes replacing %d silent samples)\n", totalBits, totalBytes, numCodeChanges, numSilentRuns, totalSilentSamples);


    // Histogram
    memset(m_deltaHistogram, 0, 256);   // Clear all entries to 0 to start with
    for (int i=0; i<numDeltasToEncode; i++) {
        int8_t delta = m_8BitDeltaBuffer[i];
        m_deltaHistogram[(uint8_t)delta]++;       
    }


*/



/* 

// Old 8BitVbrDelta Direct Encoding code... didn't work very well.

    // Write out the scaling value and first raw sample
    *outputBytes++ = m_8BitScaledBuffer[0]; // Scale value
    *outputBytes++ = m_8BitScaledBuffer[1]; // First raw sample
    // Increment output bytes count
    (*outNumBytesOutputData) += 2;

    uint32_t inputIndex = 0; // Index of the sample or delta being compressed
    uint32_t outputBitIndex = 0; // Track our output data bit position

    // Keep going until we have encoded all the input samples
//    printf("Starting to encode bit-packed delta and silent codes...\n");
    while (inputIndex < numDeltasToEncode) {

        uint8_t code = 99;
        uint8_t numSamplesEncoded = 0;

        printf("Input Sample Index: %d\n", inputIndex);
        // First, check if our current sample is 0. If so, we need to count how many there are,
        // in case we can use a silent run code.
        uint8_t numSilentSamples = 0;
        for (int i=inputIndex+1; i<numDeltasToEncode; i++) {
            if (m_8BitScaledBuffer[i] == 0) numSilentSamples++; else break;
            if (numSilentSamples == 16) break;
        }
        if (numSilentSamples > 0) {
            // We have a silent run. Encode it and move on.
            printf("Found a silent run of %d samples.\n", numSilentSamples);
            if (numSilentSamples >= 4) { code = 0; numSamplesEncoded = 4; }
            else if (numSilentSamples == 16) { code = 1; numSamplesEncoded = 16; }
            write_nbits(outputBitIndex, outputBytes, 3, code);
            outputBitIndex += 3;
        }
        else {
            // We haven't got a silent run, so we need to search ahead and try to compress deltas.
            // First, get the initial delta for this run.
            int8_t delta = m_8BitDeltaBuffer[inputIndex];
            // Calculate what delta length we need to encode it
            uint8_t minDeltaBits = count_significant_bits(delta);
            printf("Starting to encode a run of delta samples. First delta: %d, numBits: %d\n", delta, minDeltaBits);
            // Now search ahead until we hit a delta that needs more bits.
            uint32_t runLength = 1;
            for (int i=inputIndex+1; i<numDeltasToEncode; i++) {
                delta = m_8BitDeltaBuffer[i];
                uint8_t deltaBits = count_significant_bits(delta);
                printf("Scanning run: sample[%d]: %d, delta = %d, numBits = %d\n", i, (int8_t)m_8BitScaledBuffer[i+1], delta, deltaBits);
                if (deltaBits > minDeltaBits) break;
                runLength++;
            }
            printf("Found a run of %d deltas that fit in %d bits. Encoding...\n", runLength, minDeltaBits);
            // Output the run to the bit-packed output buffer
            code = minDeltaBits - 1;
            write_nbits(outputBitIndex, outputBytes, 3, code); // Write the delta run length code
            outputBitIndex += 3;
            for (int i=0; i<runLength; i++) {
                write_nbits(outputBitIndex, outputBytes, minDeltaBits, m_8BitDeltaBuffer[inputIndex+i]); // Write the delta
                outputBitIndex += minDeltaBits;
            }
            // Only output the end of run code if this is NOT the last run.
            if (inputIndex + runLength < numDeltasToEncode) {
                write_nbits(outputBitIndex, outputBytes, minDeltaBits, 1 << code); // Write the end of run code            
                outputBitIndex += minDeltaBits;
            }
            numSamplesEncoded = runLength;
        }
        inputIndex += numSamplesEncoded;
        printf("Encoded %d samples so far. %d samples/deltas remaining to be encoded.\n", inputIndex, numDeltasToEncode - inputIndex);
    }

    // Add the number of bit-packed bytes on to the total outputted bytes.
    printf("Finished encoding... Written %d bits of packed data.\n", outputBitIndex);
    (*outNumBytesOutputData) += (outputBitIndex+7) >> 3;



*/