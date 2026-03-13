#ifndef __MAKE_TEST_AUDIO_HPP__
#define __MAKE_TEST_AUDIO_HPP__ 1

#include "../../Codecs/AudioCodec.hpp"

template <typename AudioSampleType> 
class SynthesiseAudio {
public:
    void sineWave(float frequency, float sampleRate, float amplitude, AudioSampleType *outputBuffer, uint32_t numSamples);
    void noise(float sampleRate, float amplitude, AudioSampleType *outputBuffer, uint32_t numSamples);
};


#endif // __MAKE_TEST_AUDIO_HPP__