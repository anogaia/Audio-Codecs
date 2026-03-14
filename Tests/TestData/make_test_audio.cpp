#include "make_test_audio.hpp"
#include <math.h>
#include <random>

template <class AudioSampleType>
void SynthesiseAudio<AudioSampleType>::sineWave(float frequency, float sampleRate, float amplitude, AudioSampleType *outputBuffer, uint32_t numSamples) {

    float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();
    float samplePeriod = 1.0f / sampleRate;
    float angleStep = (2*M_PI * frequency) * samplePeriod;
    float angle = 0.0f;

    for (int i=0; i<numSamples; i++) {
        AudioSampleType value = (AudioSampleType)(amplitude * sinf(angle) * fullScale);
//        printf("sineWave: %0.2f\r\n", (float)value);
        *outputBuffer++ = value;
        angle += angleStep;
    }
}


template <class AudioSampleType>
void SynthesiseAudio<AudioSampleType>::noise(float sampleRate, float amplitude, AudioSampleType *outputBuffer, uint32_t numSamples) {
    // Create a random number generator
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator

    // Define the range
    std::uniform_real_distribution<> distr(amplitude * -1.0, amplitude); // Define the distribution

    float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();

    for (int i=0; i<numSamples; i++) {
        AudioSampleType value = (AudioSampleType)(distr(eng) * fullScale);
        *outputBuffer++ = value;
    }
}

// Explicit instantiation to keep Clang++ linker happy
template class SynthesiseAudio<AudioF32>;
template class SynthesiseAudio<AudioF64>;
template class SynthesiseAudio<AudioS8>;
template class SynthesiseAudio<AudioS16>;
template class SynthesiseAudio<AudioS32>;


