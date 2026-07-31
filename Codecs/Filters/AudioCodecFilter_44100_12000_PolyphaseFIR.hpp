#pragma once

#include "../AudioCodec.hpp"

/**
 *
 * Polyphase FIR rational resampler — 44.1 kHz <-> 12 kHz (40:147)
 *
 * Design
 * ------
 *   Method             : Parks-McClellan equiripple (remez)
 *   Prototype rate     : 1764000 Hz  (= 40 * 44100 = 147 * 12000)
 *   Total taps         : 5880  (linear-phase)
 *   Encode phases (L)  : 40   taps/phase : 147
 *   Decode phases (M)  : 147  taps/phase : 40
 *   Remez stop weight  : 10
 *
 * Measured frequency response (prototype / L, relative to 0 dB passband)
 * ---------------------------
 *   Passband edge      : 5500 Hz
 *   Passband deviation : 0.051 dB
 *   Passband ripple    : 0.060 dB
 *   Transition band    : 5500 - 6500 Hz
 *   Stopband edge      : 6500 Hz
 *   Stopband atten     : 67.4 dB
 *
 * Trade-off note
 * --------------
 * Same absolute pass/stop edges as the 48 kHz 4:1 design.  The stopband
 * edge (6500 Hz) coincides with the start of the first alias zone of the
 * 12 kHz domain (fs_out - f_pass = 12000 - 5500 = 6500 Hz).
 *
 * Exact ratio: 12000/44100 = 40/147.  Streaming state carries the
 * fractional remainder so long runs average to the true rate; callers
 * should use getDecimatedSamples()/getExpandedSamples() (state-aware)
 * for buffer sizing — floor(n*40/147) alone is not sufficient across
 * packet boundaries.
 *
 * Regenerate coefficients:
 *   source tools/.venv/bin/activate && python tools/design_fir_44k1_12k.py --emit-header
 *
 */

template <typename AudioSampleType>
class AudioCodecFilter_44100_12000_PolyphaseFIR : public AudioCodecFilter<AudioSampleType> {
public:
    static constexpr int L = 40;   // encode interp / decode decimate
    static constexpr int M = 147;  // encode decimate / decode interp
    static constexpr int ENC_TAPS = 147;
    static constexpr int DEC_TAPS = 40;

private:
    /* Encode (44.1 kHz -> 12 kHz): delay of ENC_TAPS input samples. */
    struct EncState {
        float    delay[ENC_TAPS];
        uint32_t head;       /* next write index */
        uint64_t in_count;   /* total input samples consumed */
        uint64_t out_index;  /* next output index m */
    };

    /* Decode (12 kHz -> 44.1 kHz): delay of DEC_TAPS low-rate samples. */
    struct DecState {
        float    delay[DEC_TAPS];
        uint32_t head;
        uint64_t in_count;
        uint64_t out_index;
    };

    EncState m_enc;
    DecState m_dec;

    float convolve_enc(uint32_t phase, uint64_t n0) const;
    float convolve_dec(uint32_t phase, uint64_t n0) const;

public:
    void reset() override {
        memset(&m_enc, 0, sizeof(m_enc));
        memset(&m_dec, 0, sizeof(m_dec));
    }

    void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples,
                  AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    void expand(AudioSampleType *inputSamples, uint32_t numInputSamples,
                AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;

    uint32_t getDecimatedSamples(uint32_t inputSamples) override;
    uint32_t getExpandedSamples(uint32_t inputSamples) override;

    AudioCodecFilter_44100_12000_PolyphaseFIR() { reset(); }
    ~AudioCodecFilter_44100_12000_PolyphaseFIR() override = default;
};
