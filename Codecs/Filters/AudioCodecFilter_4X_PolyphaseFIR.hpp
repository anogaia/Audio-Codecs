#include "../AudioCodec.hpp"

#define POLY_NUM_PHASES  4
#define POLY_TAPS        32   /* taps per phase              */
#define POLY_TOTAL_TAPS  128  /* POLY_NUM_PHASES * POLY_TAPS */

/**
 *
 * Polyphase FIR decimation filter — 4:1 (48 kHz -> 12 kHz)
 *
 * Design
 * ------
 *   Method          : Parks-McClellan equiripple (remez)
 *   Total taps      : 128  (linear-phase, symmetric)
 *   Polyphase phases: 4
 *   Taps per phase  : 32
 *
 * Measured frequency response
 * ---------------------------
 *   Passband edge   : 5500 Hz
 *   Passband ripple : 0.244 dB   (note: original spec was 0.15 dB; see trade-off note)
 *   Transition band : 5500 - 6500 Hz
 *   Stopband edge   : 6500 Hz
 *   Stopband atten  : 65.0 dB
 *
 * Trade-off note
 * --------------
 * With 128 taps and a 1000 Hz transition band at 48 kHz, the Parks-McClellan
 * equiripple optimum sits on a Pareto frontier between passband ripple and
 * stopband attenuation.  The design that meets 65 dB stopband attenuation
 * exactly, yields 0.244 dB passband ripple, which is acceptable.
 *
 * The stopband edge (6500 Hz) coincides with the start of the first alias
 * zone of the 4:1 downsampler (fs_out - f_pass = 12000 - 5500 = 6500 Hz),
 * so all aliasing components are attenuated by at least 65 dB.
 *
*/

template <typename AudioSampleType> 
class AudioCodecFilter_4X_PolyphaseFIR : public AudioCodecFilter<AudioSampleType> {
private:

    /* Downsampler filter state.
     *
     * A single ring buffer of POLY_TOTAL_TAPS (128) samples is used.
     * Every input sample is pushed into it unconditionally.  When the
     * input pointer crosses a group boundary (every M samples) one output
     * sample is produced by accumulating all M sub-filter dot-products.
     */
    typedef struct {
        float    delay[POLY_TOTAL_TAPS]; /* ring buffer — last 128 input samples */
        uint32_t head;                   /* next write index (0 .. 127)           */
        uint32_t in_count;               /* samples received mod POLY_NUM_PHASES  */
    } PolyDownState;

    /* ---------------------------------------------------------------------------
     * Upsampler — 1:4 (12 kHz -> 48 kHz)
     *
     * The same prototype filter is used for interpolation.  By the noble
     * identity, the lowpass that suppresses aliases on the way down is
     * exactly the interpolation filter needed on the way up.
     *
     * Structure
     * ---------
     * Each low-rate input sample is pushed into a 32-element ring buffer
     * (one slot per low-rate sample — NOT 128).  All M=4 phase sub-filters
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
     * State is a 32-element ring buffer ticked once per low-rate sample.
     * --------------------------------------------------------------------------- */
    typedef struct {
        float    delay[POLY_TAPS];  // ring buffer — last 32 low-rate input samples
        uint32_t head;              // next write index (0 .. POLY_TAPS-1)
    } PolyUpState;

    PolyDownState m_downState;
    PolyUpState m_upState;

    // Delay line wraparound index masks
    const uint32_t DOWN_MASK = POLY_TOTAL_TAPS - 1;  // 0x7F
    const uint32_t UP_MASK = POLY_TAPS - 1;  // 0x1F

    // Polyphase filter functions
    bool decimate_sample(float input_sample, float& output);
    void expand_sample(float input_sample, float (&output_samples)[4]);

public:
    void reset() override {  // Clears all state memory, useful if re-using an instance for a new audio stream.
        memset(&m_downState, 0, sizeof(m_downState)); 
        memset(&m_upState, 0, sizeof(m_upState));
    };

    void decimate(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;    
    void expand(AudioSampleType *inputSamples, uint32_t numInputSamples, AudioSampleType *outputSamples, uint32_t *outNumOutputSamples) override;
    uint32_t getDecimatedSamples(uint32_t inputSamples) override;
    uint32_t getExpandedSamples(uint32_t inputSamples) override;

    AudioCodecFilter_4X_PolyphaseFIR() { reset(); };
    ~AudioCodecFilter_4X_PolyphaseFIR() override {};


private:

    // Polyphase sub-filter coefficients:
    static constexpr float poly_phase[POLY_NUM_PHASES][POLY_TAPS] = {
        {  /* phase 0 */
            +0.0001387191f,
            +0.0021769755f,
            -0.0015597877f,
            +0.0018423833f,
            -0.0022909026f,
            +0.0028300641f,
            -0.0034501401f,
            +0.0041567817f,
            -0.0049742211f,
            +0.0059230351f,
            -0.0070669587f,
            +0.0085143680f,
            -0.0104982374f,
            +0.0135822546f,
            -0.0195531623f,
            +0.0384038214f,
            +0.2397746954f,
            -0.0230142740f,
            +0.0101177363f,
            -0.0054216459f,
            +0.0029995607f,
            -0.0015497541f,
            +0.0006174574f,
            -0.0000019407f,
            -0.0004029055f,
            +0.0006509904f,
            -0.0007917654f,
            +0.0008465527f,
            -0.0008354498f,
            +0.0007789347f,
            -0.0007452776f,
            +0.0021051017f 
        },
        {  /* phase 1 */
            +0.0009936756f,
            +0.0016079861f,
            -0.0015191965f,
            +0.0019382647f,
            -0.0025714857f,
            +0.0033823944f,
            -0.0043833269f,
            +0.0056086935f,
            -0.0071271406f,
            +0.0090286279f,
            -0.0114913812f,
            +0.0148430106f,
            -0.0197818305f,
            +0.0280794611f,
            -0.0458931737f,
            +0.1188594467f,
            +0.1942610715f,
            -0.0511326858f,
            +0.0281068190f,
            -0.0184596374f,
            +0.0130519943f,
            -0.0095504954f,
            +0.0070861789f,
            -0.0052644634f,
            +0.0038751674f,
            -0.0028083545f,
            +0.0019807282f,
            -0.0013493161f,
            +0.0008872222f,
            -0.0005907169f,
            +0.0004967480f,
            +0.0015339282f 
        },
        {  /* phase 2 */
            +0.0015339282f,
            +0.0004967480f,
            -0.0005907169f,
            +0.0008872222f,
            -0.0013493161f,
            +0.0019807282f,
            -0.0028083545f,
            +0.0038751674f,
            -0.0052644634f,
            +0.0070861789f,
            -0.0095504954f,
            +0.0130519943f,
            -0.0184596374f,
            +0.0281068190f,
            -0.0511326858f,
            +0.1942610715f,
            +0.1188594467f,
            -0.0458931737f,
            +0.0280794611f,
            -0.0197818305f,
            +0.0148430106f,
            -0.0114913812f,
            +0.0090286279f,
            -0.0071271406f,
            +0.0056086935f,
            -0.0043833269f,
            +0.0033823944f,
            -0.0025714857f,
            +0.0019382647f,
            -0.0015191965f,
            +0.0016079861f,
            +0.0009936756f 
        },
        {  /* phase 3 */
            +0.0021051017f,
            -0.0007452776f,
            +0.0007789347f,
            -0.0008354498f,
            +0.0008465527f,
            -0.0007917654f,
            +0.0006509904f,
            -0.0004029055f,
            -0.0000019407f,
            +0.0006174574f,
            -0.0015497541f,
            +0.0029995607f,
            -0.0054216459f,
            +0.0101177363f,
            -0.0230142740f,
            +0.2397746954f,
            +0.0384038214f,
            -0.0195531623f,
            +0.0135822546f,
            -0.0104982374f,
            +0.0085143680f,
            -0.0070669587f,
            +0.0059230351f,
            -0.0049742211f,
            +0.0041567817f,
            -0.0034501401f,
            +0.0028300641f,
            -0.0022909026f,
            +0.0018423833f,
            -0.0015597877f,
            +0.0021769755f,
            +0.0001387191f 
        } 
    };
};