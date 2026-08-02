#include "AudioCodecFilter_44100_12000_PolyphaseFIR.hpp"

#include "AudioCodecFilter_44100_12000_PolyphaseFIR_coeffs.inc"

// ---------------------------------------------------------------------------
// Sample-count helpers (state-aware)
//
// After consuming `inputSamples` more inputs, output index m is available
// when floor(m * decimation / interpolation) <= final_in - 1, i.e.
//   m <= (final_in * interpolation - 1) / decimation
// ---------------------------------------------------------------------------

template <class AudioSampleType>
uint32_t AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::getDecimatedSamples(
    uint32_t inputSamples) {
    const uint64_t final_in = m_enc.in_count + (uint64_t)inputSamples;
    if (final_in == 0) {
        return 0;
    }
    const uint64_t last_m = (final_in * (uint64_t)L - 1) / (uint64_t)M;
    if (last_m < m_enc.out_index) {
        return 0;
    }
    return (uint32_t)(last_m - m_enc.out_index + 1);
}

template <class AudioSampleType>
uint32_t AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::getExpandedSamples(
    uint32_t inputSamples) {
    // Decode: interp M, decimate L  (out/in = M/L = 147/40)
    const uint64_t final_in = m_dec.in_count + (uint64_t)inputSamples;
    if (final_in == 0) {
        return 0;
    }
    const uint64_t last_m = (final_in * (uint64_t)M - 1) / (uint64_t)L;
    if (last_m < m_dec.out_index) {
        return 0;
    }
    return (uint32_t)(last_m - m_dec.out_index + 1);
}

// ---------------------------------------------------------------------------
// Polyphase convolutions
//
// Encode: y[m] = sum_k poly_enc[ρ][k] * x[n0 - k]
//   n0 = floor(m * M / L),  ρ = (m * M) % L
// Decode: same with L/M swapped and poly_dec (pre-scaled by M/L).
// ---------------------------------------------------------------------------

template <class AudioSampleType>
float AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::convolve_enc(
    uint32_t phase, uint64_t n0) const {
    const float *coef = poly_enc[phase];
    // Newest stored sample is index (in_count - 1).  Distance back to n0:
    const uint64_t newest = m_enc.in_count - 1;
    const uint32_t back_from_newest = (uint32_t)(newest - n0);
    uint32_t rd = (m_enc.head + ENC_TAPS - 1 - back_from_newest) % ENC_TAPS;

    float acc = 0.0f;
    for (int k = 0; k < ENC_TAPS; k++) {
        acc += coef[k] * m_enc.delay[rd];
        rd = (rd + ENC_TAPS - 1) % ENC_TAPS;  // step older
    }
    return acc;
}

template <class AudioSampleType>
float AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::convolve_dec(
    uint32_t phase, uint64_t n0) const {
    const float *coef = poly_dec[phase];
    const uint64_t newest = m_dec.in_count - 1;
    const uint32_t back_from_newest = (uint32_t)(newest - n0);
    uint32_t rd = (m_dec.head + DEC_TAPS - 1 - back_from_newest) % DEC_TAPS;

    float acc = 0.0f;
    for (int k = 0; k < DEC_TAPS; k++) {
        acc += coef[k] * m_dec.delay[rd];
        rd = (rd + DEC_TAPS - 1) % DEC_TAPS;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Decimate — 44.1 kHz -> 12 kHz
// ---------------------------------------------------------------------------

template <class AudioSampleType>
void AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::decimate(
    AudioSampleType *inputSamples, uint32_t numInputSamples,
    AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    const uint32_t maxOut = *outNumOutputSamples;
    const float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();
    uint32_t out_i = 0;
    uint32_t in_i = 0;

    while (out_i < maxOut) {
        const uint64_t n0 = (m_enc.out_index * (uint64_t)M) / (uint64_t)L;
        // Need samples through index n0 inclusive.
        while (m_enc.in_count <= n0 && in_i < numInputSamples) {
            m_enc.delay[m_enc.head] = (float)(inputSamples[in_i] / fullScale);
            m_enc.head = (m_enc.head + 1) % ENC_TAPS;
            m_enc.in_count++;
            in_i++;
        }
        if (m_enc.in_count <= n0) {
            break;  // need more input
        }

        const uint32_t phase = (uint32_t)((m_enc.out_index * (uint64_t)M) % (uint64_t)L);
        const float y = convolve_enc(phase, n0);
        outputSamples[out_i++] = (AudioSampleType)(y * fullScale);
        m_enc.out_index++;
    }

    // Consume any remaining input that fits without producing further output
    // beyond maxOut — still advance the delay line so state stays consistent
    // when the caller sized the buffer via getDecimatedSamples().
    while (in_i < numInputSamples) {
        // If another output would be generated, stop so we don't desync
        // getDecimatedSamples / compress counts.
        const uint64_t n0_next = (m_enc.out_index * (uint64_t)M) / (uint64_t)L;
        if (m_enc.in_count > n0_next) {
            // An output is already producible — only skip if out buffer full.
            if (out_i >= maxOut) {
                break;
            }
            const uint32_t phase = (uint32_t)((m_enc.out_index * (uint64_t)M) % (uint64_t)L);
            const float y = convolve_enc(phase, n0_next);
            outputSamples[out_i++] = (AudioSampleType)(y * fullScale);
            m_enc.out_index++;
            continue;
        }
        m_enc.delay[m_enc.head] = (float)(inputSamples[in_i] / fullScale);
        m_enc.head = (m_enc.head + 1) % ENC_TAPS;
        m_enc.in_count++;
        in_i++;
    }

    *outNumOutputSamples = out_i;
}

// ---------------------------------------------------------------------------
// Expand — 12 kHz -> 44.1 kHz
// ---------------------------------------------------------------------------

template <class AudioSampleType>
void AudioCodecFilter_44100_12000_PolyphaseFIR<AudioSampleType>::expand(
    AudioSampleType *inputSamples, uint32_t numInputSamples,
    AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) {

    const uint32_t maxOut = *outNumOutputSamples;
    const float fullScale = AudioCodecUtils::fullScaleValue<AudioSampleType>();
    uint32_t out_i = 0;
    uint32_t in_i = 0;

    while (out_i < maxOut) {
        // Decode uses interp=M, decim=L
        const uint64_t n0 = (m_dec.out_index * (uint64_t)L) / (uint64_t)M;
        while (m_dec.in_count <= n0 && in_i < numInputSamples) {
            m_dec.delay[m_dec.head] = (float)(inputSamples[in_i] / fullScale);
            m_dec.head = (m_dec.head + 1) % DEC_TAPS;
            m_dec.in_count++;
            in_i++;
        }
        if (m_dec.in_count <= n0) {
            break;
        }

        const uint32_t phase = (uint32_t)((m_dec.out_index * (uint64_t)L) % (uint64_t)M);
        const float y = convolve_dec(phase, n0);
        outputSamples[out_i++] = (AudioSampleType)(y * fullScale);
        m_dec.out_index++;
    }

    while (in_i < numInputSamples) {
        const uint64_t n0_next = (m_dec.out_index * (uint64_t)L) / (uint64_t)M;
        if (m_dec.in_count > n0_next) {
            if (out_i >= maxOut) {
                break;
            }
            const uint32_t phase = (uint32_t)((m_dec.out_index * (uint64_t)L) % (uint64_t)M);
            const float y = convolve_dec(phase, n0_next);
            outputSamples[out_i++] = (AudioSampleType)(y * fullScale);
            m_dec.out_index++;
            continue;
        }
        m_dec.delay[m_dec.head] = (float)(inputSamples[in_i] / fullScale);
        m_dec.head = (m_dec.head + 1) % DEC_TAPS;
        m_dec.in_count++;
        in_i++;
    }

    *outNumOutputSamples = out_i;
}

// Explicit instantiations
template class AudioCodecFilter_44100_12000_PolyphaseFIR<AudioF32>;
template class AudioCodecFilter_44100_12000_PolyphaseFIR<AudioF64>;
template class AudioCodecFilter_44100_12000_PolyphaseFIR<AudioS8>;
template class AudioCodecFilter_44100_12000_PolyphaseFIR<AudioS16>;
template class AudioCodecFilter_44100_12000_PolyphaseFIR<AudioS32>;
