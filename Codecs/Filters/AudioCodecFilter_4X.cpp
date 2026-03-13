#include "AudioCodecFilter_4X.hpp"

// 4X Decimate - Downsample by a factor of 4, so 48kHz would end up as 12kHz etc.
// This is a filter-and-decimate operation, so it has the effect of removing the above-nyquist frequencies at 
// the same time as it downsamples the audio content to the lower sample rate. 
template <class AudioSampleType> 
void AudioCodecFilter_4X<AudioSampleType>::decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    // Limit the number of input samples we can process, to make sure we don't overrun the buffer that was provided
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples*4);
    // Calculate the actual number of output samples that will be written to the buffer
    uint32_t numOutputSamples = numInputSamples/4;

    // Try to offset the filter window so that it is best centred in the coefficients
    int32_t numCoefficients = sizeof(m_coefficients) / sizeof(m_coefficients[0]);
    int32_t filterOffset = 2 - numCoefficients/2;

    for (uint32_t i=0; i<numOutputSamples; i++) {
        // For each output sample, we multiply each input sample in the window by its coefficient and average the result.
        AudioSampleType accumulator = 0;
        float sumCoefficients = 0.0f;
        for (int32_t c=0; c<numCoefficients; c++) {
            uint32_t index = i * 4 + filterOffset + c;
            if (index >=0 && index < numInputSamples) {
                accumulator += inputSamples[index] * m_coefficients[c];
                sumCoefficients += m_coefficients[c];
            }
        }
        // Store the filtered output sample
        // We rescale the accumulated value here. Normally, that isn't needed, since the coefficients
        // all add up to 1.0, but since we may miss one due to overrunning the input data, we have to rescale
        // based on how many coefficients and samples we actually used.
        // To make this work, we just need to record the total of the coefficients we actually used and divide by that.
        // If we use all of them, it'll add up to 1.0 and dividing by 1.0 doesn't change it.
        // If we used less, it'll add up to less than 1.0 and dividing by the actual total renormalises to 1.0.
        // This also has the nice side effect that even if our coefficients don't add up to 1.0, it'll still work.
        outputSamples[i] = accumulator / sumCoefficients;
    }

    // Report samples written
    *outNumOutputSamples = numOutputSamples;
}

// 4X Expansion - Upsample by factor of 4, hopefully without adding unwanted harmonics.
// To do this, a fairly simple linear interpolation is used.
template <class AudioSampleType>
void AudioCodecFilter_4X<AudioSampleType>::expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {
    // Limit the number of input samples we can process, to make sure we don't overrun the buffer that was provided
    numInputSamples = std::min(numInputSamples, *outNumOutputSamples/4);
    // Calculate the actual number of output samples that will be written to the buffer
    uint32_t numOutputSamples = numInputSamples*4;

    // Expand using linear interpolation, placing the new samples at index +2, same as we got them from the input filter.
    // This should keep the phase pretty accurate.
    for (uint32_t i=0; i<numInputSamples; i++) {

        AudioSampleType inputSample = inputSamples[i];
        *outputSamples++ = m_lastSample * 0.75f + inputSample * 0.25f;
        *outputSamples++ = m_lastSample * 0.5f + inputSample * 0.5f;
        *outputSamples++ = m_lastSample * 0.25f + inputSample * 0.75f;
        *outputSamples++ = inputSample;

        // Remember our last input sample, so we can use it for the next loop and keep it for the next packet, too.
        m_lastSample = inputSample;
    }

    // Report samples written
    *outNumOutputSamples = numOutputSamples;
}    

template <class AudioSampleType>
uint32_t AudioCodecFilter_4X<AudioSampleType>::getDecimatedSamples(uint32_t inputSamples) { return inputSamples/4; }

template <class AudioSampleType>
uint32_t AudioCodecFilter_4X<AudioSampleType>::getExpandedSamples(uint32_t inputSamples) { return inputSamples*4; }


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecFilter_4X<AudioF32>;
template class AudioCodecFilter_4X<AudioF64>;
template class AudioCodecFilter_4X<AudioS8>;
template class AudioCodecFilter_4X<AudioS16>;
template class AudioCodecFilter_4X<AudioS32>;
