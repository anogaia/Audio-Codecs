#ifndef __AUDIOCODECUTILS_HPP__
#define __AUDIOCODECUTILS_HPP__ 1

#include <stdint.h>
#include <cstring>
#include <algorithm>
#include <limits>
#include <math.h>
#include <type_traits>
#include <typeinfo>

struct AudioCodecUtils {

// Utility inline function to determine full scale value of different sample types.
template <typename AudioSampleType>
static inline float fullScaleValue() {
    if (std::is_integral<AudioSampleType>::value) {
        // Integer type - full scale is simply the maximum value for the type
        return (float)std::numeric_limits<AudioSampleType>::max();
    } else {
        // Floating point type - assume 1.0 is full scale (-1.0 to +1.0)
        // This is fairly universal, but you can change this value if needs arise.
        return 1.0f;
    }
};

// Sign-extend a value that has less than 8 bits to a full, signed 8-bit value.
static inline int8_t sign_extend(uint8_t value, unsigned n_bits) {
    // Mask to isolate the n_bits bits
    uint8_t mask = (1 << n_bits) - 1;
    uint8_t val = value & mask;

    // Extract sign bit and move it to bit 0
    uint8_t sign_bit = (val >> (n_bits - 1)) & 1;

    // Create mask: 0x00 if sign_bit=0, 0xFF if sign_bit=1
    uint8_t sign_mask = 0 - sign_bit;  // subtraction creates all 1s if sign_bit=1

    // Mask for bits above n_bits
    uint8_t extend_mask = ~mask;

    // OR the extended bits with original value
    return (int8_t)(val | (sign_mask & extend_mask));
};

// Sign-extend a value of 1..16 bits to a signed 16-bit value (used by 12BitVbrDelta).
static inline int16_t sign_extend16(uint16_t value, unsigned n_bits) {
    if (n_bits == 0 || n_bits > 16) return 0;
    uint16_t mask = (uint16_t)((n_bits == 16) ? 0xFFFFu : ((1u << n_bits) - 1u));
    uint16_t val = value & mask;
    uint16_t sign_bit = (val >> (n_bits - 1)) & 1u;
    uint16_t sign_mask = (uint16_t)(0u - sign_bit);
    uint16_t extend_mask = (uint16_t)~mask;
    return (int16_t)(val | (sign_mask & extend_mask));
};

// Calculate the number of significant bits required to encode a signed 8-bit value
// Modified to return at least 3 bits, to match the 8BitVbrDelta codec
static inline uint8_t count_significant_bits(int8_t num) {
    uint8_t magnitude = num < 0 ? -num : num;
    uint8_t count = 2;

    // Count bits in the magnitude
    if (magnitude >= 64) count = 7; 
    else if (magnitude >= 32) count = 6;
    else if (magnitude >= 16) count = 5;
    else if (magnitude >= 8)  count = 4;
    else if (magnitude >= 4)  count = 3;
//    else if (magnitude >= 2)  count = 2;

    return count + 1;
};

// Calculate the number of significant bits required to encode a signed value up to 12-bit.
// Returns at least 3 bits and at most 12 bits, to match the 12BitVbrDelta codec.
static inline uint8_t count_significant_bits12(int16_t num) {
    uint16_t magnitude = (uint16_t)(num < 0 ? -num : num);
    uint8_t count = 2;

    if (magnitude >= 1024) count = 11;
    else if (magnitude >= 512) count = 10;
    else if (magnitude >= 256) count = 9;
    else if (magnitude >= 128) count = 8;
    else if (magnitude >= 64)  count = 7;
    else if (magnitude >= 32)  count = 6;
    else if (magnitude >= 16)  count = 5;
    else if (magnitude >= 8)   count = 4;
    else if (magnitude >= 4)   count = 3;

    return count + 1;
};

// Read n bits (from 1 to 8) from a bit-packed data buffer at a specific bit index
// Note: This treats data as LSB first. For example, reading 3 bits at index 0 would get the 3 least significant bits of that byte.
static inline uint8_t read_nbits(uint32_t bit_index, uint8_t *data, unsigned n_bits) {
    // Check that n_bits is within valid range.
    if (n_bits < 1 || n_bits > 8) {
        return 0; // Return 0 for invalid number of bits.
    }

    // Calculate the byte index and the bit position within that byte using bit shifts.
    uint32_t byte_index = bit_index >> 3; // Equivalent to bit_index / 8
    uint32_t bit_pos = bit_index & 0x07;   // Equivalent to bit_index % 8

    // Read the byte containing the bits we want.
    uint8_t byte_value = data[byte_index];

    // Create a mask to extract the required bits.
    uint8_t mask = (1 << n_bits) - 1;

    // If there are bits spanning two bytes
    if (bit_pos + n_bits > 8) {
        uint8_t next_byte_value = data[byte_index + 1];
        return ((byte_value >> bit_pos) | (next_byte_value << (8 - bit_pos))) & mask;
    }

    // If the bits are within a single byte
    return (byte_value >> bit_pos) & mask;
};

// Write n_bits into a data buffer at the specified bit index
// Note: This treats data as LSB first. For example, writing 3 bits at index 0 would set the 3 least significant bits of that byte.
static inline void write_nbits(uint32_t bit_index, uint8_t *data, unsigned n_bits, uint8_t bits) {
    // Check that n_bits is within valid range
    if (n_bits < 1 || n_bits > 8) {
        return; // Do nothing for invalid input
    }

    // Calculate the byte index and the bit position within that byte using bit shifts
    uint32_t byte_index = bit_index >> 3; // Equivalent to bit_index / 8
    uint32_t bit_pos = bit_index & 0x07;   // Equivalent to bit_index % 8

    // Prepare the mask for the bits to write
    uint8_t mask = (1 << n_bits) - 1;

    // Clear the bits in the target position
    data[byte_index] &= ~(mask << bit_pos);

    // Write the bits into the buffer
    data[byte_index] |= (bits & mask) << bit_pos;

    // If writing bits spans into the next byte
    if (bit_pos + n_bits > 8) {
        data[byte_index + 1] &= ~(mask >> (8 - bit_pos)); // Clear relevant bits in the next byte
        data[byte_index + 1] |= (bits & mask) >> (8 - bit_pos); // Write remaining bits to next byte
    }
};

// Read 1..16 bits from a bit-packed data buffer (LSB-first). Used by 12BitVbrDelta.
static inline uint16_t read_nbits16(uint32_t bit_index, uint8_t *data, unsigned n_bits) {
    if (n_bits < 1 || n_bits > 16) {
        return 0;
    }
    if (n_bits <= 8) {
        return read_nbits(bit_index, data, n_bits);
    }
    // Split into low 8 bits + remaining high bits
    uint16_t low = read_nbits(bit_index, data, 8);
    uint16_t high = read_nbits(bit_index + 8, data, n_bits - 8);
    return (uint16_t)(low | (high << 8));
};

// Write 1..16 bits into a data buffer (LSB-first). Used by 12BitVbrDelta.
static inline void write_nbits16(uint32_t bit_index, uint8_t *data, unsigned n_bits, uint16_t bits) {
    if (n_bits < 1 || n_bits > 16) {
        return;
    }
    if (n_bits <= 8) {
        write_nbits(bit_index, data, n_bits, (uint8_t)bits);
        return;
    }
    write_nbits(bit_index, data, 8, (uint8_t)(bits & 0xFF));
    write_nbits(bit_index + 8, data, n_bits - 8, (uint8_t)((bits >> 8) & 0xFF));
};


// Diagonal Mapping helper functions

static inline uint8_t diag_map(int8_t value) {
    // Map negative values to even indices, positive values to odd indices, and preserve zero
    if (value < 0) {
        // Map negative values to even indices (e.g., -1 -> 2, -2 -> 4)
        return (uint8_t)(-value * 2); // Negative values
    } else if (value > 0) {
        // Map positive values to odd indices (e.g., +1 -> 1, +2 -> 3)
        return (uint8_t)(value * 2 - 1); // Positive values
    } else {
        return 0; // Preserve zero
    }
};

static inline int8_t diag_unmap(uint8_t value) {
    // Reverse the mapping
    if (value % 2 == 0) {
        return (int8_t)(-((int)value / 2)); // Negative values
    } else {
        return (int8_t)((int)value / 2 + 1); // Positive values
    }
};

};

// rANS (range Asymmetric Numeral Systems) Compression

#include <iostream>
#include <vector>
#include <cstdint>
#include <cassert>
#include <array>

// Constants for rANS
constexpr uint32_t ANS_L = 1u << 16; // 16-bit renormalization lower bound
constexpr uint32_t PROB_SCALE = 1u << 12; // Probability scale (4096)
constexpr uint32_t MAX_SYMBOLS = 256; // Alphabet size (e.g., 8-bit symbols)

// Adaptive frequency table
struct AdaptiveFreqTable {
    std::array<double, MAX_SYMBOLS> float_freq{};       // symbol frequencies
    std::array<uint16_t, MAX_SYMBOLS> freq{};       // symbol frequencies
    std::array<uint16_t, MAX_SYMBOLS + 1> cum_freq{}; // cumulative frequencies

    uint32_t total_freq = 0;

    AdaptiveFreqTable() {
        // Initialize frequencies to PROB_SCALE/MAX_SYMBOLS, to ensure they add up to PROB_SCALE
        double frequency = 64.0;
        double offset = 3.0;
        for (int i=0; i<MAX_SYMBOLS; i++) {
            float_freq[i] = frequency - offset + 1.0;
            frequency = (frequency - offset) * 0.95 + offset;
            if (frequency < offset) frequency = offset;
        }
        for (int i=0; i<float_freq.size(); i++) {
//            printf("Initial Frequency[%d] = %0.2f\n", i, float_freq[i]);
        }
        normalise();
    }

    void normalise() {
        // First step is to get the total frequency and then scale frequencies so that they 
        // approximate the desired probability scale
        double t_freq = 0.0;
        for (auto& f : float_freq) t_freq += f; 
        double scale = PROB_SCALE / (double)t_freq;
        for (auto& f : float_freq) f *= scale;
        for (int i=0; i<freq.size(); i++) {
            freq[i] = (uint16_t)(float_freq[i] + 0.499);
//            printf("Scaled Frequency[%d] = %d\n", i, freq[i]);
        }
        // Rebuild cumulative total, to see how close we got
        build_cumulative();
        int32_t diff = PROB_SCALE - total_freq;
//        printf("Normalise: Difference = %d\n", diff);
        // Distribute difference to correct to exact probability scale
        // With an overshoot, we'll subtract some from the end of the symbol table,
        // and oppositely, if we undershoot, we'll add some to the beginning, since
        // we're mainly going for a histogram that favours the lower end of the symbol space.
        if (diff > 0) {
            // We have to add some to the lower end
            for (auto& f : freq) {
                f++;
                diff--;
                if (diff == 0) break;
            }
        } else if (diff < 0) {
            for (int i=freq.size()-1; i>=0; i--) {
                freq[i] -= 1;
                diff++;
                if (diff == 0) break;
            }
        }
        // Finally, rebuild the cumulative table again
        build_cumulative();
        printf("Normalise: Cumulative total = %u, Probability scale = %u\n", total_freq, PROB_SCALE);
        for (int i=0; i<freq.size(); i++) {
//            printf("Final Frequency[%d] = %d\n", i, freq[i]);
        }
    }

    void build_cumulative() {
        cum_freq[0] = 0;
        for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
            cum_freq[i + 1] = cum_freq[i] + freq[i];
        }
        total_freq = cum_freq[MAX_SYMBOLS];
    }

    // Update frequency of symbol and rebuild cumulative frequencies periodically
    void update(uint8_t symbol) {
        if (freq[symbol] < 0xFFFF) {
            freq[symbol]++;
        } else {
            // Scale down frequencies to avoid overflow
            for (auto& f : freq) {
                f = (f + 1) >> 1;
                if (f == 0) f = 1;
            }
        }
        build_cumulative();
    }
};



// rANS Encoder class
class rANSEncoder {
    uint32_t state;

public:
    rANSEncoder() : state(ANS_L) {}

    // Encode a symbol given its cumulative freq start and freq
    void encode(uint16_t start, uint16_t freq, std::vector<uint8_t>& output) {
        // Renormalize
        while (state >= ((ANS_L >> 12) * freq)) {
            output.push_back(state & 0xFF);
            state >>= 8;
        }
        // Encode symbol
        state = ((state / freq) << 12) + (state % freq) + start;
    }

    // Flush remaining state bytes to output
    void flush(std::vector<uint8_t>& output) {
        for (int i = 0; i < 4; ++i) {
            output.push_back(state & 0xFF);
            state >>= 8;
        }
    }
};

// rANS Decoder class
class rANSDecoder {
    uint32_t state;
    size_t pos;

public:
    rANSDecoder(const std::vector<uint8_t>& input) : state(0), pos(input.size() - 4) {
        // Initialize state from last 4 bytes
        for (int i = 3; i >= 0; --i) {
            state = (state << 8) | input[pos + i];
        }
    }

    // Decode a symbol from state and update state
    uint8_t decode(const std::vector<uint8_t>& input, const AdaptiveFreqTable& table) {
        uint32_t x = state & (PROB_SCALE - 1);

        // Find symbol for x using binary search on cumulative frequencies
        size_t low = 0, high = MAX_SYMBOLS;
        while (low < high) {
            size_t mid = (low + high) / 2;
            if (table.cum_freq[mid + 1] > x) {
                high = mid;
            } else {
                low = mid + 1;
            }
        }
        uint8_t symbol = static_cast<uint8_t>(low);

        uint16_t start = table.cum_freq[symbol];
        uint16_t freq = table.freq[symbol];

        // Update state
        state = freq * (state >> 12) + (x - start);

        // Renormalize
        while (state < ANS_L && pos > 0) {
            --pos;
            state = (state << 8) | input[pos];
        }
        return symbol;
    }
};

inline std::string stripFileExtension(std::string fileName) {
    size_t dotPosition = fileName.find_last_of('.');
    if (dotPosition != std::string::npos) {
        return fileName.substr(0, dotPosition);
    }
    return fileName;
}


#endif // __AUDIOCODECUTILS_HPP__