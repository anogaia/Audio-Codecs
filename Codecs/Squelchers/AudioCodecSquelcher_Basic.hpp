#include "../AudioCodec.hpp"

// AudioCodecSquelcher_Basic - A basic implementation of a squelcher
template <typename AudioSampleType> 
class AudioCodecSquelcher_Basic : public AudioCodecSquelcher<AudioSampleType> {
protected:

    float m_currentSquelch;
    float m_threshold, m_attackRate, m_decayRate;

    float m_inputAvg = 1.0f;

public:
    void squelch(AudioSampleType *inputSamples, uint32_t numInputSamples) override;

    AudioCodecSquelcher_Basic(float threshold = 0.0035f, float attackRate = 0.75f, float decayRate = 0.9f);

     ~AudioCodecSquelcher_Basic() override {};
};
