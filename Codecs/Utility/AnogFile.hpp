#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

// ANOG (.anog) container — see docs/ANOG_FORMAT.md
// Explicit little-endian packing (no bitfields).

namespace Anog {

static constexpr char kMagic[4] = {'A', 'N', 'O', 'G'};
static constexpr uint16_t kVersion = 1;
static constexpr uint16_t kHeaderSize = 64;
static constexpr uint32_t kCompressedRate = 12000;

enum CodecId : uint8_t {
    Codec_8BitVbrDelta = 1,
};

enum FilterId : uint8_t {
    Filter_48k_4X_Polyphase = 1,
    Filter_44100_12000_Polyphase = 2,
};

struct Header {
    char     magic[4] = {'A', 'N', 'O', 'G'};
    uint16_t version = kVersion;
    uint16_t header_size = kHeaderSize;
    uint8_t  channels = 1;
    uint8_t  codec_id = Codec_8BitVbrDelta;
    uint8_t  filter_id = Filter_48k_4X_Polyphase;
    uint8_t  flags = 0;
    uint32_t pcm_sample_rate = 48000;
    uint32_t compressed_rate = kCompressedRate;
    uint16_t frame_ms = 100;
    uint16_t reserved0 = 0;
    uint64_t pcm_total_samples = 0;
    uint32_t seek_entry_count = 0;
    uint64_t compressed_total_samples = 0;
    // padding[20] written as zeros
};

struct SeekEntry {
    uint64_t tick = 0;
    uint64_t file_offset = 0;
};

inline void write_u16_le(std::ostream &os, uint16_t v) {
    char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
    os.write(b, 2);
}

inline void write_u32_le(std::ostream &os, uint32_t v) {
    char b[4] = {
        char(v & 0xff), char((v >> 8) & 0xff),
        char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
    os.write(b, 4);
}

inline void write_u64_le(std::ostream &os, uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = char((v >> (8 * i)) & 0xff);
    }
    os.write(b, 8);
}

inline bool read_u16_le(std::istream &is, uint16_t &v) {
    unsigned char b[2];
    if (!is.read(reinterpret_cast<char *>(b), 2)) return false;
    v = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
    return true;
}

inline bool read_u32_le(std::istream &is, uint32_t &v) {
    unsigned char b[4];
    if (!is.read(reinterpret_cast<char *>(b), 4)) return false;
    v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

inline bool read_u64_le(std::istream &is, uint64_t &v) {
    unsigned char b[8];
    if (!is.read(reinterpret_cast<char *>(b), 8)) return false;
    v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t(b[i]) << (8 * i));
    }
    return true;
}

inline bool write_header(std::ostream &os, const Header &h) {
    os.write(h.magic, 4);
    write_u16_le(os, h.version);
    write_u16_le(os, h.header_size);
    os.put(char(h.channels));
    os.put(char(h.codec_id));
    os.put(char(h.filter_id));
    os.put(char(h.flags));
    write_u32_le(os, h.pcm_sample_rate);
    write_u32_le(os, h.compressed_rate);
    write_u16_le(os, h.frame_ms);
    write_u16_le(os, h.reserved0);
    write_u64_le(os, h.pcm_total_samples);
    write_u32_le(os, h.seek_entry_count);
    write_u64_le(os, h.compressed_total_samples);
    char pad[20] = {};
    os.write(pad, 20);
    return bool(os);
}

inline bool read_header(std::istream &is, Header &h) {
    if (!is.read(h.magic, 4)) return false;
    if (std::memcmp(h.magic, kMagic, 4) != 0) return false;
    if (!read_u16_le(is, h.version)) return false;
    if (!read_u16_le(is, h.header_size)) return false;
    int c = is.get();
    if (c < 0) return false;
    h.channels = uint8_t(c);
    c = is.get();
    if (c < 0) return false;
    h.codec_id = uint8_t(c);
    c = is.get();
    if (c < 0) return false;
    h.filter_id = uint8_t(c);
    c = is.get();
    if (c < 0) return false;
    h.flags = uint8_t(c);
    if (!read_u32_le(is, h.pcm_sample_rate)) return false;
    if (!read_u32_le(is, h.compressed_rate)) return false;
    if (!read_u16_le(is, h.frame_ms)) return false;
    if (!read_u16_le(is, h.reserved0)) return false;
    if (!read_u64_le(is, h.pcm_total_samples)) return false;
    if (!read_u32_le(is, h.seek_entry_count)) return false;
    if (!read_u64_le(is, h.compressed_total_samples)) return false;
    char pad[20];
    if (!is.read(pad, 20)) return false;
    if (h.version != kVersion) return false;
    if (h.header_size != kHeaderSize) return false;
    return true;
}

inline bool write_seek_entry(std::ostream &os, const SeekEntry &e) {
    write_u64_le(os, e.tick);
    write_u64_le(os, e.file_offset);
    return bool(os);
}

inline bool read_seek_entry(std::istream &is, SeekEntry &e) {
    return read_u64_le(is, e.tick) && read_u64_le(is, e.file_offset);
}

inline bool write_seek_table(std::ostream &os, const std::vector<SeekEntry> &table) {
    for (const auto &e : table) {
        if (!write_seek_entry(os, e)) return false;
    }
    return true;
}

inline bool read_seek_table(std::istream &is, uint32_t count, std::vector<SeekEntry> &table) {
    table.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        if (!read_seek_entry(is, table[i])) return false;
    }
    return true;
}

inline bool write_packet(std::ostream &os, uint8_t channel, uint16_t comp_samples,
                         const uint8_t *payload, uint16_t payload_len) {
    write_u16_le(os, payload_len);
    os.put(char(channel));
    write_u16_le(os, comp_samples);
    if (payload_len) {
        os.write(reinterpret_cast<const char *>(payload), payload_len);
    }
    return bool(os);
}

inline bool read_packet(std::istream &is, uint8_t &channel, uint16_t &comp_samples,
                        std::vector<uint8_t> &payload) {
    uint16_t payload_len = 0;
    if (!read_u16_le(is, payload_len)) return false;
    int c = is.get();
    if (c < 0) return false;
    channel = uint8_t(c);
    if (!read_u16_le(is, comp_samples)) return false;
    payload.resize(payload_len);
    if (payload_len && !is.read(reinterpret_cast<char *>(payload.data()), payload_len)) {
        return false;
    }
    return true;
}

inline uint64_t packet_stream_offset(const Header &h) {
    return uint64_t(h.header_size) + uint64_t(h.seek_entry_count) * 16ull;
}

inline uint32_t count_frames(uint64_t pcm_total_samples, uint32_t pcm_sample_rate, uint16_t frame_ms) {
    if (pcm_total_samples == 0 || pcm_sample_rate == 0 || frame_ms == 0) {
        return 0;
    }
    const double duration_ms = (double(pcm_total_samples) * 1000.0) / double(pcm_sample_rate);
    uint32_t n = uint32_t(duration_ms / double(frame_ms));
    if (double(n) * double(frame_ms) < duration_ms) {
        n++;
    }
    if (n == 0) {
        n = 1;
    }
    return n;
}

inline uint64_t pcm_index_at_frame(uint32_t frame_index, uint16_t frame_ms, uint32_t pcm_sample_rate,
                                   uint64_t pcm_total_samples) {
    if (frame_index == 0) {
        return 0;
    }
    const uint64_t idx =
        uint64_t(llround((double(frame_index) * double(frame_ms) * double(pcm_sample_rate)) / 1000.0));
    return idx < pcm_total_samples ? idx : pcm_total_samples;
}

}  // namespace Anog
