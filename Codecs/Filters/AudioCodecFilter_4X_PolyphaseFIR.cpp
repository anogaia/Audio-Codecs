#include "AudioCodecFilter_4X_PolyphaseFIR.hpp"


// 4X Decimate - Downsample by a factor of 4, so 48kHz would end up as 12kHz etc.
// This is a filter-and-decimate operation, so it has the effect of removing the above-nyquist frequencies at 
// the same time as it downsamples the audio content to the lower sample rate. 
template <class AudioSampleType> 
void AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // Limit the number of input samples we can process, to make sure we don't overrun the buffer that was provided
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples*4);
    // Calculate the actual number of output samples that will be written to the buffer
    uint32_t numOutputSamples = numInputSamples/4;

    float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();
    uint32_t outputSampleIndex = 0;
    float decimatedSample;

    for (uint32_t i=0; i<numInputSamples; i++) {
        float sample = (float)(inputSamples[i] / fullScale);
        if (decimate_sample(sample, decimatedSample)) {
            outputSamples[outputSampleIndex++] = (AudioSampleType)(decimatedSample * fullScale);
        }
    }
    
    // Report samples written
    *outNumOutputSamples = outputSampleIndex;
}


// 4X Expansion - Upsample by factor of 4, using the same FIR filter in reverse
template <class AudioSampleType>
void AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {
    // Limit the number of input samples we can process, to make sure we don't overrun the buffer that was provided
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples/4);
    // Calculate the actual number of output samples that will be written to the buffer
    uint32_t numOutputSamples = numInputSamples*4;

    float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();
    uint32_t outputSampleIndex = 0;
    float expandedSamples[4];

    for (uint32_t i=0; i<numInputSamples; i++) {
        float sample = (float)(inputSamples[i] / fullScale);
        expand_sample(sample, expandedSamples);
        outputSamples[outputSampleIndex++] = (AudioSampleType)(expandedSamples[0] * fullScale);
        outputSamples[outputSampleIndex++] = (AudioSampleType)(expandedSamples[1] * fullScale);
        outputSamples[outputSampleIndex++] = (AudioSampleType)(expandedSamples[2] * fullScale);
        outputSamples[outputSampleIndex++] = (AudioSampleType)(expandedSamples[3] * fullScale);
    }

    // Report samples written
    *outNumOutputSamples = outputSampleIndex;
}    


template <class AudioSampleType>
uint32_t AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::getDecimatedSamples(uint32_t inputSamples) { return inputSamples/4; }

template <class AudioSampleType>
uint32_t AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::getExpandedSamples(uint32_t inputSamples) { return inputSamples*4; }


// Process incoming samples and generate decimated output, 1 output per 4 input samples
// Returns true if a valid output was produced, false otherwise
template <class AudioSampleType>
bool AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::decimate_sample(float input_sample, float& output) {

    bool outValid = false;
    PolyDownState &s = m_downState;
    
    // Push the new sample into the 128-element ring buffer.
    s.delay[s.head] = input_sample;
    s.head = (s.head + 1) & DOWN_MASK;

    // Produce one output sample at the start of each new group of 4.
    if (s.in_count == 0) {
        float acc = 0.0f;

        // Accumulate all M=4 sub-filter dot-products.
        // Sub-filter m starts at (head - 1 - m), stepping back by 4 taps.
        for (int m = 0; m < POLY_NUM_PHASES; m++) {
            const float *coef = poly_phase[m];
            uint32_t     rd   = (s.head - 1 - m) & DOWN_MASK;

            for (int j = 0; j < POLY_TAPS; j++) {
                acc += coef[j] * s.delay[rd];
                rd   = (rd - POLY_NUM_PHASES) & DOWN_MASK;
            }
        }
        output = acc;
        outValid = true;    // Signal caller that this time, the output is a valid sample
    }

    // Advance phase counter (cycles 0 -> 1 -> 2 -> 3 -> 0 ...).
    s.in_count = (s.in_count + 1) & (POLY_NUM_PHASES - 1);
    return outValid;
}


// Process one incoming sample and generate 4 output samples
template <class AudioSampleType>
void AudioCodecFilter_4X_PolyphaseFIR<AudioSampleType>::expand_sample(float input_sample, float (&output_samples)[4]) {

    PolyUpState &s = m_upState;

    // Push one low-rate sample into the 32-element ring buffer.
    s.delay[s.head] = input_sample;
    s.head = (s.head + 1) & UP_MASK;

    // Apply all (M=4) sub-filters to produce M consecutive output samples.    
    for (int m = 0; m < POLY_NUM_PHASES; m++) {
        // Phases run from PHASES-1 down to 0 so the output group is in causal order.
        const float *coef = poly_phase[POLY_NUM_PHASES - 1 - m]; 
        uint32_t     rd   = s.head;  // oldest sample
        float        acc  = 0.0f;

        for (int j = 0; j < POLY_TAPS; j++) {
            acc += coef[j] * s.delay[rd];
            rd   = (rd + 1) & UP_MASK;
        }
        output_samples[m] = (float)POLY_NUM_PHASES * acc;
    }
}



// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecFilter_4X_PolyphaseFIR<AudioF32>;
template class AudioCodecFilter_4X_PolyphaseFIR<AudioF64>;
template class AudioCodecFilter_4X_PolyphaseFIR<AudioS8>;
template class AudioCodecFilter_4X_PolyphaseFIR<AudioS16>;
template class AudioCodecFilter_4X_PolyphaseFIR<AudioS32>;