#include "AudioCodecCompressor_12BitVbrDelta.hpp"

// 12-Bit VBR Audio Compressor
// Uses variable bit-length (equivalent to VBR) delta coding to compress a 12-bit audio stream.
// Analogous to AudioCodecCompressor_8BitVbrDelta, but preserves 12-bit dynamic range.
//
// Packet layout:
//   Byte 0       : unsigned 8-bit scale value (same encoding as 8/12BitScaled)
//   Bytes 1-2    : first raw sample as little-endian int16 (12-bit magnitude)
//   Remaining    : bit-packed VBR delta / silence codes
//
// The first 4 bits are a delta size indicator (wider than the 8-bit codec's 3-bit codes
// so that lengths up to 12 bits can be represented):
// 0  = 4 silent samples
// 1  = 16 silent samples
// 2  = 3-bit delta samples (-3 to +3) follow
// 3  = 4-bit delta samples (-7 to +7) follow
// ...
// 11 = 12-bit delta samples (-2047 to +2047) follow
//
// Codes 12-15 are reserved / unused.
//
// The unsymmetric most-negative value of each delta size is used as an end-of-run marker
// (e.g. -2048 for 12-bit deltas). A delta of exactly -2048 is adjusted to -2047 with
// forward error propagation, matching the 8-bit codec's -128 handling.


// Prototypes for internal helper functions (12-bit variants; names avoid clashing with 8-bit helpers)
static void prepareDeltaAndCodeBuffers12(uint32_t numDeltas, int16_t *samples, int16_t *deltas, uint8_t *deltaLengths, uint8_t *codes);
static void prepareBitLengthChanges12(uint32_t numDeltas, uint8_t *deltaLengths, uint8_t *codes, const uint8_t *minRun);
static void prepareSilentRuns12(uint32_t numDeltas, int16_t *samples, uint8_t *codes, const uint8_t *minRun);

static inline void store_int16_le(AudioBytes *dst, int16_t value) {
    dst[0] = (AudioBytes)(value & 0xFF);
    dst[1] = (AudioBytes)((value >> 8) & 0xFF);
}

static inline int16_t load_int16_le(const AudioBytes *src) {
    return (int16_t)(src[0] | (src[1] << 8));
}


// Compress a buffer of native samples.
template <class AudioSampleType>
void AudioCodecCompressor_12BitVbrDelta<AudioSampleType>::compress(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioBytes *outputBytes, uint32_t *outNumBytesOutputData) {

    // Ask the 12BitScaled compressor how big a buffer we'll need.
    uint32_t num12BitScaledBytes = AudioCodecCompressor_12BitScaled<AudioSampleType>::getBytesMaxCompressedSize(numInputSamples);
    uint32_t numDeltasToEncode = (numInputSamples > 0) ? (numInputSamples - 1) : 0;

    if (num12BitScaledBytes > m_num12BitScaledBufferBytes || numDeltasToEncode > m_numDeltaSlots) {
        if (m_12BitScaledBuffer != NULL) free(m_12BitScaledBuffer);
        m_12BitScaledBuffer = (AudioBytes*)malloc(num12BitScaledBytes);
        m_num12BitScaledBufferBytes = num12BitScaledBytes;

        if (m_12BitSampleBuffer != NULL) free(m_12BitSampleBuffer);
        m_12BitSampleBuffer = (int16_t*)malloc(numInputSamples * sizeof(int16_t));

        if (m_12BitDeltaBuffer != NULL) free(m_12BitDeltaBuffer);
        m_12BitDeltaBuffer = (int16_t*)malloc(std::max(numDeltasToEncode, (uint32_t)1) * sizeof(int16_t));

        if (m_deltaLengthBuffer != NULL) free(m_deltaLengthBuffer);
        m_deltaLengthBuffer = (uint8_t*)malloc(std::max(numDeltasToEncode, (uint32_t)1));

        if (m_codeBuffer != NULL) free(m_codeBuffer);
        m_codeBuffer = (uint8_t*)malloc(std::max(numDeltasToEncode, (uint32_t)1));

        m_numDeltaSlots = numDeltasToEncode;
    }

    // First stage: compress to the 12-bit scaled representation
    uint32_t outBytes12BitScaledOutputData = num12BitScaledBytes;
    AudioCodecCompressor_12BitScaled<AudioSampleType>::compress(inputSamples, numInputSamples, m_12BitScaledBuffer, &outBytes12BitScaledOutputData);

    // Parse scaled samples into an int16 working buffer (skip the scale byte)
    for (uint32_t i=0; i<numInputSamples; i++) {
        m_12BitSampleBuffer[i] = load_int16_le(&m_12BitScaledBuffer[1 + i * 2]);
    }

    // Calculate deltas, their minimum bit widths for encoding and initialise the codes buffer.
    prepareDeltaAndCodeBuffers12(numDeltasToEncode, m_12BitSampleBuffer, m_12BitDeltaBuffer, m_deltaLengthBuffer, m_codeBuffer);

    // Compress bit widths wherever that uses fewer bits.
    prepareBitLengthChanges12(numDeltasToEncode, m_deltaLengthBuffer, m_codeBuffer, minRun);

    // Silent runs are always cheaper than encoding zero deltas.
    prepareSilentRuns12(numDeltasToEncode, m_12BitSampleBuffer, m_codeBuffer, minRun);

    // Write out the scaling value and the first raw sample (16-bit LE)
    *outputBytes++ = m_12BitScaledBuffer[0]; // Scale value
    store_int16_le(outputBytes, m_12BitSampleBuffer[0]);
    outputBytes += 2;

    uint32_t inputIndex = 0;
    uint32_t outputBitIndex = 0;

    while (inputIndex < numDeltasToEncode) {

        uint8_t code = m_codeBuffer[inputIndex];
        uint32_t numSamplesEncoded = 0;

        if (code < 2) {
            // Silent run
            AudioCodecUtils::write_nbits16(outputBitIndex, outputBytes, kCodeBits, code);
            outputBitIndex += kCodeBits;
            numSamplesEncoded = minRun[code];
        }
        else {
            uint8_t deltaBits = code + 1;
            uint32_t runLength = 1;
            for (uint32_t i=inputIndex+1; i<numDeltasToEncode; i++) {
                if (m_codeBuffer[i] == code) runLength++; else break;
            }
            AudioCodecUtils::write_nbits16(outputBitIndex, outputBytes, kCodeBits, code);
            outputBitIndex += kCodeBits;
            for (uint32_t i=0; i<runLength; i++) {
                AudioCodecUtils::write_nbits16(outputBitIndex, outputBytes, deltaBits, (uint16_t)m_12BitDeltaBuffer[inputIndex+i]);
                outputBitIndex += deltaBits;
            }
            // Only output the end of run code if this is NOT the last run.
            if (inputIndex + runLength < numDeltasToEncode) {
                AudioCodecUtils::write_nbits16(outputBitIndex, outputBytes, deltaBits, (uint16_t)(1u << code));
                outputBitIndex += deltaBits;
            }
            numSamplesEncoded = runLength;
        }
        inputIndex += numSamplesEncoded;
    }

    // Fixed header is 24 bits (1 scale byte + 2 sample bytes), plus packed bits.
    *outNumBytesOutputData = (outputBitIndex + 24 + 7) >> 3;
    printf("12BitVbrDelta: Finished encoding... Written %d bits of data. (%d bytes)\n", outputBitIndex + 24, *outNumBytesOutputData);
}



// Decompress a buffer of compressed samples to native samples.
// Note: *outNumOutputSamples must be set to the total number of output samples that the input data will decompress to before calling.
template <class AudioSampleType>
void AudioCodecCompressor_12BitVbrDelta<AudioSampleType>::decompress(AudioBytes *inputData, uint32_t numBytesInputData, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    uint32_t numOutputSamples = *outNumOutputSamples;
    uint32_t num12BitScaledBytes = AudioCodecCompressor_12BitScaled<AudioSampleType>::getBytesMaxCompressedSize(numOutputSamples);
    if (num12BitScaledBytes > m_num12BitScaledDecompBufferBytes) {
        if (m_12BitScaledDecompBuffer != NULL) free(m_12BitScaledDecompBuffer);
        m_12BitScaledDecompBuffer = (AudioBytes*)malloc(num12BitScaledBytes);
        m_num12BitScaledDecompBufferBytes = num12BitScaledBytes;
    }

    // Copy scale + first sample into the scaled reconstruction buffer
    m_12BitScaledDecompBuffer[0] = inputData[0];
    m_12BitScaledDecompBuffer[1] = inputData[1];
    m_12BitScaledDecompBuffer[2] = inputData[2];

    uint32_t outputSampleIndex = 1; // samples written into scaled buffer (first already present)
    uint32_t outputDataIndex = 3;   // byte index into scaled buffer

    bool modeIsDelta = false;
    uint8_t currentDeltaLength = 0;
    AudioBytes *bitPackedInputData = &(inputData[3]);
    uint32_t inputDataBitIndex = 0;
    uint16_t currentEndOfRunCode = 0;
    uint8_t silenceLengthRemaining = 0;
    int16_t currentSample = load_int16_le(&inputData[1]);

    while (outputSampleIndex < numOutputSamples) {
        if (modeIsDelta) {
            uint16_t deltaBits = AudioCodecUtils::read_nbits16(inputDataBitIndex, bitPackedInputData, currentDeltaLength);
            inputDataBitIndex += currentDeltaLength;
            if (deltaBits == currentEndOfRunCode) {
                modeIsDelta = false;
            } else {
                currentSample = (int16_t)(currentSample + AudioCodecUtils::sign_extend16(deltaBits, currentDeltaLength));
                store_int16_le(&m_12BitScaledDecompBuffer[outputDataIndex], currentSample);
                outputDataIndex += 2;
                outputSampleIndex++;
            }
        } else {
            if (silenceLengthRemaining > 0) {
                currentSample = 0;
                store_int16_le(&m_12BitScaledDecompBuffer[outputDataIndex], 0);
                outputDataIndex += 2;
                outputSampleIndex++;
                silenceLengthRemaining--;
            } else {
                uint8_t code = (uint8_t)AudioCodecUtils::read_nbits16(inputDataBitIndex, bitPackedInputData, kCodeBits);
                inputDataBitIndex += kCodeBits;
                if (code < 2) {
                    currentSample = 0;
                    silenceLengthRemaining = minRun[code];
                } else if (code <= kMaxDeltaCode) {
                    currentDeltaLength = code + 1;
                    currentEndOfRunCode = (uint16_t)(1u << code);
                    modeIsDelta = true;
                } else {
                    // Reserved codes — treat as end of stream / skip
                    break;
                }
            }
        }
    }

    // Optional decode check against the compression-side scaled buffer when available
    if (m_12BitScaledBuffer != NULL && m_num12BitScaledBufferBytes >= num12BitScaledBytes) {
        uint32_t numIncorrectDecodes = 0;
        uint32_t totalError = 0;
        for (uint32_t i=0; i<numOutputSamples; i++) {
            int16_t inputValue = load_int16_le(&m_12BitScaledBuffer[1 + i * 2]);
            int16_t outputValue = load_int16_le(&m_12BitScaledDecompBuffer[1 + i * 2]);
            if (inputValue != outputValue) {
                numIncorrectDecodes++;
                totalError += (uint32_t)std::abs(inputValue - outputValue);
            }
        }
        printf("12BitVbrDelta: Incorrect Decoded Values: %d (%0.1f %% of %d). Total error: %d\n",
               numIncorrectDecodes, 100.0f*(float)numIncorrectDecodes/numOutputSamples, numOutputSamples, totalError);
    }

    AudioCodecCompressor_12BitScaled<AudioSampleType>::decompress(m_12BitScaledDecompBuffer, outputDataIndex, outputSamples, outNumOutputSamples);
}


template <class AudioSampleType>
uint32_t AudioCodecCompressor_12BitVbrDelta<AudioSampleType>::getBytesMaxCompressedSize(uint32_t numSamples) {
    // Worst case: all deltas as 12-bit, plus scale, first sample (16 bits), plus one 4-bit length code.
    uint32_t bits = (numSamples > 0 ? (numSamples - 1) * 12 : 0) + 8 + 16 + kCodeBits;
    return (bits + 7) >> 3;
}

template <class AudioSampleType>
uint32_t AudioCodecCompressor_12BitVbrDelta<AudioSampleType>::getBytesMaxDecompressedSize(uint32_t numSamples) {
    return sizeof(AudioSampleType) * numSamples;
}


// Explicit Instantiations
template class AudioCodecCompressor_12BitVbrDelta<AudioF32>;
template class AudioCodecCompressor_12BitVbrDelta<AudioF64>;
template class AudioCodecCompressor_12BitVbrDelta<AudioS8>;
template class AudioCodecCompressor_12BitVbrDelta<AudioS16>;
template class AudioCodecCompressor_12BitVbrDelta<AudioS32>;


// Helper Function Definitions

static void prepareDeltaAndCodeBuffers12(uint32_t numDeltas, int16_t *samples, int16_t *deltas, uint8_t *deltaLengths, uint8_t *codes) {

    int16_t errorCorrection = 0;
    for (uint32_t i=0; i<numDeltas; i++) {
        int32_t thisSample = samples[i];
        int32_t nextSample = samples[i+1];
        int32_t delta = nextSample - thisSample + errorCorrection;
        errorCorrection = 0;
        int16_t delta16 = (int16_t)delta;

        // Avoid using -2048 (the 12-bit end-of-run symbol) as a real delta.
        if (delta16 == -2048) {
            printf("************* Got a naughty -2048 delta @ pos[%d]. Correcting to -2047 and propagating error to next delta.\n", i);
            delta16 = -2047;
            errorCorrection = -1;
        }

        uint8_t minDeltaBits = AudioCodecUtils::count_significant_bits12(delta16);
        deltas[i] = delta16;
        deltaLengths[i] = minDeltaBits;
        // Start with all deltas at the largest bit length (code 11 -> 12 bits)
        codes[i] = 11;
    }
}

static void prepareBitLengthChanges12(uint32_t numDeltas, uint8_t *deltaLengths, uint8_t *codes, const uint8_t *minRun) {
    for (int code=11; code>2; code--) {
        uint8_t currentDeltaLength = (uint8_t)(code + 1);
        uint32_t minRunLength = minRun[code-1];
        uint32_t runStart = 0, runLength = 0;
        for (uint32_t i=0; i<numDeltas; i++) {
            uint8_t deltaLength = deltaLengths[i];
            if (runLength > 0) {
                if (deltaLength < currentDeltaLength) runLength++;
                else {
                    if (runLength > minRunLength) {
                        for (uint32_t u=runStart; u<(runStart+runLength); u++) { codes[u] = (uint8_t)(code-1); }
                    }
                    runLength = 0;
                }
            } else {
                if (deltaLength < currentDeltaLength) { runStart = i; runLength++; }
            }
        }
        if (runLength > minRunLength) {
            for (uint32_t u=runStart; u<(runStart+runLength); u++) { codes[u] = (uint8_t)(code-1); }
        }
    }
}

static void prepareSilentRuns12(uint32_t numDeltas, int16_t *samples, uint8_t *codes, const uint8_t *minRun) {
    for (int code=1; code>=0; code--) {
        uint32_t minRunLength = minRun[code];
        uint32_t runStart = 0, runLength = 0;
        for (uint32_t i=0; i<numDeltas; i++) {
            if ((code == 0) && (codes[i] == 1)) i += 15;
            else {
                if (runLength > 0) {
                    // samples[i+1] is the "next" sample corresponding to delta i
                    if (samples[i+1] == 0) runLength++;
                    else {
                        if (runLength >= minRunLength) {
                            printf("**** 12Bit Silent Run Found ****  Inserting a %d-long run of %d-sample silent codes.\n", runLength/minRunLength, minRunLength);
                            for (uint32_t u=runStart; u<(runStart+runLength-(minRunLength-1)); u+=minRunLength) { codes[u] = (uint8_t)code; }
                        }
                        runLength = 0;
                    }
                } else {
                    if (samples[i+1] == 0) {
                        runStart = i; runLength++;
                    }
                }
            }
        }
        if (runLength > 0 && runLength == numDeltas - runStart) {
            runLength = ((runLength + minRunLength-1) / minRunLength) * minRunLength;
            printf("**** 12Bit Silent Run Found ****  Inserting a %d-long run of %d-sample silent codes.\n", runLength/minRunLength, minRunLength);
            for (uint32_t u=runStart; u<(runStart+runLength-1); u+=minRunLength) { codes[u] = (uint8_t)code; }
        }
    }
}
