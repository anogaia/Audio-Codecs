#include "../AudioCodec.hpp"

// Prefixed macros to avoid colliding with the 4X polyphase FIR defines when both headers are included.
#define POLY2X_NUM_PHASES  2
#define POLY2X_TAPS        64   /* taps per phase                   */
#define POLY2X_TOTAL_TAPS  128  /* POLY2X_NUM_PHASES * POLY2X_TAPS  */

/**
 *
 * Polyphase FIR decimation filter — 2:1 (48 kHz -> 24 kHz)
 *
 * Speech-first HD path
 * --------------------
 * Targets 48 kHz in -> 24 kHz internal -> 48 kHz out without running the
 * passband up against the 24 kHz Nyquist.  Speech content through ~8.5 kHz
 * is preserved; energy at and above 12 kHz (the new Nyquist) is rejected
 * so aliases do not fold back into the kept band.
 *
 * Design
 * ------
 *   Method          : windowed-sinc (Kaiser beta=8.0)
 *   Total taps      : 128  (linear-phase, symmetric)
 *   Polyphase phases: 2
 *   Taps per phase  : 64
 *   Design cutoff   : 10000 Hz at fs=48000  (−6 dB of the ideal sinc;
 *                     Kaiser window sets the transition shape)
 *
 * Design targets
 * --------------
 *   Passband        : through ~8500 Hz (speech band with margin)
 *   Stopband        : >= 70 dB by 12000 Hz (internal Nyquist)
 *
 * Measured frequency response (prototype FIR, approx.)
 * ----------------------------------------------------
 *   Passband to 8500 Hz : ripple < 0.01 dB (essentially flat)
 *   −0.1 dB edge        : ~9280 Hz
 *   −3 dB edge          : ~9810 Hz
 *   −70 dB from         : ~10935 Hz
 *   Attenuation @ 12 kHz: ~100 dB
 *   Stopband floor      : > 85 dB for f >= 12 kHz
 *
 * Decimate+expand cascade (|H|^2) sits a little lower: ≈ −3 dB near 9660 Hz
 * and still well past −70 dB by 12 kHz.  See the HD harness sine-sweep
 * measurement for a runtime check of the cascade response.
 *
 * Alias note
 * ----------
 * After 2:1 decimation, content above 12 kHz folds into the 24 kHz
 * representation.  With the stopband already deep by ~11 kHz, aliases
 * that would land in the speech passband are attenuated by > 70 dB.
 * This is intentionally more conservative than a near-Nyquist HD cut
 * (previous design used a ~11500 Hz cutoff / ~10500 Hz passband edge).
 *
*/

template <typename AudioSampleType>
class AudioCodecFilter_2X_PolyphaseFIR : public AudioCodecFilter<AudioSampleType> {
private:

    /* Downsampler filter state.
     *
     * A single ring buffer of POLY2X_TOTAL_TAPS (128) samples is used.
     * Every input sample is pushed into it unconditionally.  When the
     * input pointer crosses a group boundary (every M samples) one output
     * sample is produced by accumulating all M sub-filter dot-products.
     */
    typedef struct {
        float    delay[POLY2X_TOTAL_TAPS]; /* ring buffer — last 128 input samples */
        uint32_t head;                     /* next write index (0 .. 127)           */
        uint32_t in_count;                 /* samples received mod POLY2X_NUM_PHASES */
    } PolyDownState;

    /* ---------------------------------------------------------------------------
     * Upsampler — 1:2 (24 kHz -> 48 kHz)
     *
     * The same prototype filter is used for interpolation.  By the noble
     * identity, the lowpass that suppresses aliases on the way down is
     * exactly the interpolation filter needed on the way up.
     *
     * Structure
     * ---------
     * Each low-rate input sample is pushed into a 64-element ring buffer
     * (one slot per low-rate sample — NOT 128).  All M=2 phase sub-filters
     * are then applied to produce M consecutive high-rate output samples:
     *
     *   out[k*M + (M-1-m)]  =  M  *  dot( poly_phase[m], delay )
     *
     * for m = 0 .. M-1.  The factor M compensates for the energy that the
     * zero-insertion step spreads across M output slots.  Phases are output
     * in reverse order (M-1 down to 0) so that phase 0 — whose coefficients
     * are co-incident with the input sample — appears last (highest-index
     * output in the group), matching the causal direct-form ordering.
     *
     * State is a 64-element ring buffer ticked once per low-rate sample.
     * --------------------------------------------------------------------------- */
    typedef struct {
        float    delay[POLY2X_TAPS];  // ring buffer — last 64 low-rate input samples
        uint32_t head;                // next write index (0 .. POLY2X_TAPS-1)
    } PolyUpState;

    PolyDownState m_downState;
    PolyUpState m_upState;

    // Delay line wraparound index masks (TOTAL_TAPS and TAPS are powers of 2)
    const uint32_t DOWN_MASK = POLY2X_TOTAL_TAPS - 1;  // 0x7F
    const uint32_t UP_MASK = POLY2X_TAPS - 1;           // 0x3F

    // Polyphase filter functions
    bool decimate_sample(float input_sample, float& output);
    void expand_sample(float input_sample, float (&output_samples)[2]);

public:
    void reset() override {  // Clears all state memory, useful if re-using an instance for a new audio stream.
        memset(&m_downState, 0, sizeof(m_downState));
        memset(&m_upState, 0, sizeof(m_upState));
    };

    void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    void expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    uint32_t getDecimatedSamples(uint32_t inputSamples) override;
    uint32_t getExpandedSamples(uint32_t inputSamples) override;

    AudioCodecFilter_2X_PolyphaseFIR() { reset(); };
    ~AudioCodecFilter_2X_PolyphaseFIR() override {};


private:

    // Polyphase sub-filter coefficients (Kaiser beta=8, fc=10 kHz @ 48 kHz):
    // poly_phase[m][j] = h[j*2 + m] of the 128-tap linear-phase prototype.
    static constexpr float poly_phase[POLY2X_NUM_PHASES][POLY2X_TAPS] = {
        {  /* phase 0 */
            +0.0000116237f,
            -0.0000253523f,
            +0.0000315269f,
            -0.0000114142f,
            -0.0000526436f,
            +0.0001632569f,
            -0.0002937244f,
            +0.0003822169f,
            -0.0003429852f,
            +0.0000980607f,
            +0.0003764949f,
            -0.0010062502f,
            +0.0015991159f,
            -0.0018722848f,
            +0.0015337294f,
            -0.0004050478f,
            -0.0014509021f,
            +0.0036495391f,
            -0.0055016762f,
            +0.0061559862f,
            -0.0048545424f,
            +0.0012434203f,
            +0.0043544615f,
            -0.0108050849f,
            +0.0162408171f,
            -0.0183616660f,
            +0.0148902073f,
            -0.0040203987f,
            -0.0154130895f,
            +0.0446420067f,
            -0.0891477961f,
            +0.1956440899f,
            +0.3874589737f,
            -0.0165229596f,
            -0.0265648788f,
            +0.0373540517f,
            -0.0347109127f,
            +0.0252678388f,
            -0.0133922583f,
            +0.0023518766f,
            +0.0057113016f,
            -0.0098722297f,
            +0.0103211560f,
            -0.0080534550f,
            +0.0044389560f,
            -0.0007940871f,
            -0.0019347355f,
            +0.0033158714f,
            -0.0034039268f,
            +0.0025860800f,
            -0.0013772567f,
            +0.0002363165f,
            +0.0005482275f,
            -0.0008878493f,
            +0.0008541560f,
            -0.0006025369f,
            +0.0002947476f,
            -0.0000458533f,
            -0.0000948886f,
            +0.0001342057f,
            -0.0001095378f,
            +0.0000628411f,
            -0.0000233637f,
            +0.0000024348f
        },
        {  /* phase 1 */
            +0.0000024348f,
            -0.0000233637f,
            +0.0000628411f,
            -0.0001095378f,
            +0.0001342057f,
            -0.0000948886f,
            -0.0000458533f,
            +0.0002947476f,
            -0.0006025369f,
            +0.0008541560f,
            -0.0008878493f,
            +0.0005482275f,
            +0.0002363165f,
            -0.0013772567f,
            +0.0025860800f,
            -0.0034039268f,
            +0.0033158714f,
            -0.0019347355f,
            -0.0007940871f,
            +0.0044389560f,
            -0.0080534550f,
            +0.0103211560f,
            -0.0098722297f,
            +0.0057113016f,
            +0.0023518766f,
            -0.0133922583f,
            +0.0252678388f,
            -0.0347109127f,
            +0.0373540517f,
            -0.0265648788f,
            -0.0165229596f,
            +0.3874589737f,
            +0.1956440899f,
            -0.0891477961f,
            +0.0446420067f,
            -0.0154130895f,
            -0.0040203987f,
            +0.0148902073f,
            -0.0183616660f,
            +0.0162408171f,
            -0.0108050849f,
            +0.0043544615f,
            +0.0012434203f,
            -0.0048545424f,
            +0.0061559862f,
            -0.0055016762f,
            +0.0036495391f,
            -0.0014509021f,
            -0.0004050478f,
            +0.0015337294f,
            -0.0018722848f,
            +0.0015991159f,
            -0.0010062502f,
            +0.0003764949f,
            +0.0000980607f,
            -0.0003429852f,
            +0.0003822169f,
            -0.0002937244f,
            +0.0001632569f,
            -0.0000526436f,
            -0.0000114142f,
            +0.0000315269f,
            -0.0000253523f,
            +0.0000116237f
        }
    };
};
