#ifndef __AUDIOFILEUTILS_HPP__
#define __AUDIOFILEUTILS_HPP__ 1

#include <stdint.h>

// Legacy VBIF prototype (.vbi) — superseded by the ANOG container (.anog).
// See AnogFile.hpp and docs/ANOG_FORMAT.md.

struct AudioFileUtils {

    struct vbif_fs {
        // 8 or 16 bit samples (& deltas). 0 = 8 bit, 1 = 16 bit.
        uint8_t sampleBitWidth8or16 : 1;
    };

    struct vbif_header {
        char        cc4[4] = {'V', 'B', 'I', 'F'};
        vbif_fs     fileSpec;
        uint8_t     sampleRateDiv300;
        uint16_t    frameLength;
    };

    static inline vbif_header Make_VBIF_Header(uint8_t sampleBitWidth, uint32_t sampleRate, uint32_t frameLength) {
        vbif_header newHeader;
        newHeader.fileSpec.sampleBitWidth8or16 = (sampleBitWidth == 16) ? 1 : 0;
        newHeader.sampleRateDiv300 = (uint8_t)(sampleRate / 300);
        newHeader.frameLength = frameLength;
        return newHeader;
    }
};

#endif // __AUDIOFILEUTILS_HPP__
