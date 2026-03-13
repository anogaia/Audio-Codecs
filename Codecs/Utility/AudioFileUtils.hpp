#ifndef __AUDIOFILEUTILS_HPP__
#define __AUDIOFILEUTILS_HPP__ 1

#include <stdint.h>

struct AudioFileUtils {

    struct vbif_fs {
        // 8 or 16 bit samples (& deltas). 0 = 8 bit, 1 = 16 bit.
        // If 8 bit, then each frame starts with a scale factor, also 8 bits, that is stored as (real_scale_factor-1).
        // If 16 bit, then there is no scale factor. The samples are output as-is, without re-scaling.
        // In both cases, the first sample is raw, followed by encoded delta samples or silence codes.
        uint8_t   sampleBitWidth8or16 : 1;
    };


    struct vbif_header {
        char        cc4[4] = {'V', 'B', 'I', 'F'};  // 4CC VBIF ID
        vbif_fs     fileSpec;           // Encoding flags etc.
        uint8_t     sampleRateDiv300;   // Sample rate divided by 300. (eg. 160 for 48kHz, 147 for 44.1kHz etc.)
        uint16_t    frameLength;        // Length of each frame, in samples.
    };

    static inline vbif_header Make_VBIF_Header(uint8_t sampleBitWidth, uint32_t sampleRate, uint32_t frameLength) {
        vbif_header newHeader;
        newHeader.fileSpec.sampleBitWidth8or16 = (sampleBitWidth == 16) ? 1 : 0;
        newHeader.sampleRateDiv300 = (uint8_t)(sampleRate / 300);
        newHeader.frameLength = frameLength;
        return newHeader;
    };


};




#endif // __AUDIOFILEUTILS_HPP__