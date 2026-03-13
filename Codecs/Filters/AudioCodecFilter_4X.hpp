#include "../AudioCodec.hpp"


template <typename AudioSampleType> 
class AudioCodecFilter_4X : public AudioCodecFilter<AudioSampleType> {
private:
    // 5 coefficient filter. Note: All coefficients must sum to 1.0 for normalisation.
    static constexpr float m_coefficients[5] = { 0.1f, 0.25f, 0.3f, 0.25f, 0.1f };

    // Storage for last sample we processed when expanding.
    // This allows us to smoothly expand between packets of audio.
    // Obviously, this only works if the audio is expanded sequentially.
    AudioSampleType m_lastSample = 0;

public:
    void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;    
    void expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    uint32_t getDecimatedSamples(uint32_t inputSamples) override;
    uint32_t getExpandedSamples(uint32_t inputSamples) override;

    ~AudioCodecFilter_4X() override {};
};


