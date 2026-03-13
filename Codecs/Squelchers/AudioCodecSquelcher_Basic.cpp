#include "AudioCodecSquelcher_Basic.hpp"

// AudioCodecSquelcher_Basic - A basic implementation of a squelcher that has no effect
template <typename AudioSampleType> 
void AudioCodecSquelcher_Basic<AudioSampleType>::squelch(AudioSampleType *inputSamples, uint32_t numInputSamples) {

    float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    float lowerThreshold = m_threshold / 3.0f;

    bool display = (m_currentSquelch > 0.25f);

//    printf("AudioCodecSquelcher_Basic: processing %d input samples.\n", numInputSamples);

    for (int i=0; i<numInputSamples; i++) {
        AudioSampleType input = inputSamples[i];

        float inputLevel = fabs(input / fullScale);

        // Windowed average over 8 samples
        m_inputAvg = (m_inputAvg * 7.0f + inputLevel) / 8.0f;

        if (m_inputAvg < lowerThreshold) {
            // Input is below threshold, so reduce squelch factor to move towards silence.
            m_currentSquelch *= m_decayRate;
        } else if (m_inputAvg > m_threshold) {
            // Input is above threshold, so increase squelch factor to move towards full volume
            m_currentSquelch += (1.0f - m_currentSquelch) * m_attackRate;
        }

    //    if (i%16 == 0) printf("Squelch[%d]: %0.3f\n", i, m_currentSquelch);

        inputSamples[i] = (AudioSampleType)(input * m_currentSquelch);
    }

}

template <typename AudioSampleType>
AudioCodecSquelcher_Basic<AudioSampleType>::AudioCodecSquelcher_Basic(float threshold, float attackRate, float decayRate) {
    m_threshold = threshold;
    m_attackRate = attackRate;
    m_decayRate = decayRate;
    m_currentSquelch = 1.0f;
};


// Explicit Instantiations - Workaround for Clang++ linker symbols not being found in vtable.
// Use all the Audio Sample Types we defined in the common header file.
template class AudioCodecSquelcher_Basic<AudioF32>;
template class AudioCodecSquelcher_Basic<AudioF64>;
template class AudioCodecSquelcher_Basic<AudioS8>;
template class AudioCodecSquelcher_Basic<AudioS16>;
template class AudioCodecSquelcher_Basic<AudioS32>;