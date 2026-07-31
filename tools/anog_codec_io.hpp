#pragma once

#include "AudioCodec.hpp"
#include "Compressors/AudioCodecCompressor_8BitVbrDelta.hpp"
#include "Filters/AudioCodecFilter_4X_PolyphaseFIR.hpp"
#include "Filters/AudioCodecFilter_44100_12000_PolyphaseFIR.hpp"
#include "Squelchers/AudioCodecSquelcher_Basic.hpp"
#include "Utility/AnogFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace AnogCodec {

using Sample = AudioS16;

// 8BitVbrDelta is validated around VOIP packet sizes (~256 compressed samples).
static constexpr uint32_t kMaxVbrChunkSamples = 256;

struct ChannelEncoder {
    std::unique_ptr<AudioCodecFilter<Sample>> filter;
    AudioCodecCompressor_8BitVbrDelta<Sample> compressor;
    AudioCodecSquelcher_Basic<Sample> squelcher;

    void setup(Anog::FilterId filter_id) {
        if (filter_id == Anog::Filter_48k_4X_Polyphase) {
            filter = std::make_unique<AudioCodecFilter_4X_PolyphaseFIR<Sample>>();
        } else if (filter_id == Anog::Filter_44100_12000_Polyphase) {
            filter = std::make_unique<AudioCodecFilter_44100_12000_PolyphaseFIR<Sample>>();
        } else {
            throw std::runtime_error("unsupported filter_id");
        }
        filter->reset();
    }
};

inline Anog::FilterId filter_for_rate(uint32_t pcm_rate) {
    if (pcm_rate == 48000) return Anog::Filter_48k_4X_Polyphase;
    if (pcm_rate == 44100) return Anog::Filter_44100_12000_Polyphase;
    throw std::runtime_error("unsupported PCM sample rate (need 44100 or 48000)");
}

inline std::string encode_wav_channels_to_anog(
    const std::vector<std::vector<Sample>> &channels,
    uint32_t pcm_sample_rate,
    uint16_t frame_ms,
    const std::string &out_path) {

    if (channels.empty()) {
        throw std::runtime_error("no channels");
    }
    const uint8_t num_ch = uint8_t(channels.size());
    const uint64_t pcm_total = channels[0].size();
    for (uint8_t c = 1; c < num_ch; c++) {
        if (channels[c].size() != pcm_total) {
            throw std::runtime_error("channel length mismatch");
        }
    }
    if (frame_ms == 0) {
        throw std::runtime_error("frame_ms must be > 0");
    }

    const Anog::FilterId filter_id = filter_for_rate(pcm_sample_rate);
    const uint32_t frame_count_est = Anog::count_frames(pcm_total, pcm_sample_rate, frame_ms);

    std::vector<ChannelEncoder> encoders(num_ch);
    for (auto &e : encoders) {
        e.setup(filter_id);
    }

    Anog::Header hdr;
    hdr.channels = num_ch;
    hdr.codec_id = Anog::Codec_8BitVbrDelta;
    hdr.filter_id = uint8_t(filter_id);
    hdr.pcm_sample_rate = pcm_sample_rate;
    hdr.compressed_rate = Anog::kCompressedRate;
    hdr.frame_ms = frame_ms;
    hdr.pcm_total_samples = pcm_total;
    hdr.seek_entry_count = frame_count_est;
    hdr.compressed_total_samples = 0;

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open output: " + out_path);
    }

    if (!Anog::write_header(out, hdr)) {
        throw std::runtime_error("failed writing header");
    }
    std::vector<Anog::SeekEntry> seek(frame_count_est);
    if (!Anog::write_seek_table(out, seek)) {
        throw std::runtime_error("failed writing placeholder seek table");
    }

    uint64_t tick = 0;
    const uint32_t max_pcm_frame =
        uint32_t(llround((double(frame_ms) * double(pcm_sample_rate)) / 1000.0)) + 8;
    const uint32_t max_comp_frame = encoders[0].filter->getDecimatedSamples(max_pcm_frame) + 8;
    const uint32_t max_chunk_bytes =
        encoders[0].compressor.getBytesMaxCompressedSize(kMaxVbrChunkSamples) + 64;

    std::vector<Sample> pcm_scratch(max_pcm_frame);
    std::vector<Sample> mid_scratch(max_comp_frame + 8);
    std::vector<uint8_t> enc_scratch(max_chunk_bytes);

    uint32_t frames_written = 0;
    for (uint32_t fi = 0; fi < frame_count_est; fi++) {
        const uint64_t pcm_start = Anog::pcm_index_at_frame(fi, frame_ms, pcm_sample_rate, pcm_total);
        const uint64_t pcm_end = Anog::pcm_index_at_frame(fi + 1, frame_ms, pcm_sample_rate, pcm_total);
        const uint32_t n_pcm = uint32_t(pcm_end - pcm_start);
        if (n_pcm == 0) {
            continue;
        }
        if (n_pcm > max_pcm_frame) {
            throw std::runtime_error("frame PCM larger than scratch");
        }

        seek[frames_written].tick = tick;
        seek[frames_written].file_offset = uint64_t(out.tellp());

        uint32_t n_comp_ref = 0;
        for (uint8_t ch = 0; ch < num_ch; ch++) {
            for (uint32_t i = 0; i < n_pcm; i++) {
                pcm_scratch[i] = channels[ch][size_t(pcm_start + i)];
            }
            encoders[ch].squelcher.squelch(pcm_scratch.data(), n_pcm);

            const uint32_t n_comp = encoders[ch].filter->getDecimatedSamples(n_pcm);
            if (ch == 0) {
                n_comp_ref = n_comp;
            } else if (n_comp != n_comp_ref) {
                throw std::runtime_error("channel compressed length mismatch");
            }
            if (n_comp > mid_scratch.size()) {
                mid_scratch.resize(n_comp + 8);
            }
            uint32_t written_comp = n_comp;
            encoders[ch].filter->decimate(pcm_scratch.data(), n_pcm, mid_scratch.data(), &written_comp);
            if (written_comp != n_comp) {
                throw std::runtime_error("decimate wrote unexpected sample count");
            }

            uint32_t offset = 0;
            while (offset < n_comp) {
                const uint32_t chunk = std::min(kMaxVbrChunkSamples, n_comp - offset);
                const uint32_t need = encoders[ch].compressor.getBytesMaxCompressedSize(chunk) + 16;
                if (enc_scratch.size() < need) {
                    enc_scratch.resize(need);
                }
                uint32_t nbytes = uint32_t(enc_scratch.size());
                encoders[ch].compressor.compress(mid_scratch.data() + offset, chunk, enc_scratch.data(),
                                                 &nbytes);
                if (nbytes > 0xffffu) {
                    throw std::runtime_error("packet exceeds u16 payload_len");
                }
                if (!Anog::write_packet(out, ch, uint16_t(chunk), enc_scratch.data(), uint16_t(nbytes))) {
                    throw std::runtime_error("failed writing packet");
                }
                offset += chunk;
            }
        }
        tick += n_comp_ref;
        frames_written++;
    }

    if (frames_written == 0) {
        throw std::runtime_error("no frames written");
    }
    if (frames_written != frame_count_est) {
        // Rebuild file with correct seek_entry_count (rare: empty trailing slots from rounding).
        seek.resize(frames_written);
        hdr.seek_entry_count = frames_written;
    }

    hdr.compressed_total_samples = tick;
    out.flush();
    out.close();

    // If frame count shrank, rewrite the whole prefix: header + seek + we must shift packets.
    // For the common case frames_written == frame_count_est, just patch in place.
    if (frames_written == frame_count_est) {
        std::fstream patch(out_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!patch) {
            throw std::runtime_error("failed to reopen for seek table patch");
        }
        if (!Anog::write_header(patch, hdr)) {
            throw std::runtime_error("failed rewriting header");
        }
        if (!Anog::write_seek_table(patch, seek)) {
            throw std::runtime_error("failed rewriting seek table");
        }
        patch.flush();
    } else {
        // Re-encode once with exact frame count (simplest correctness).
        // Adjust count_frames result by writing only non-empty; re-run is heavy —
        // instead rewrite file from a temp packet capture. For v1, force exact
        // estimate by ensuring count_frames matches non-empty frames.
        throw std::runtime_error("internal: empty frame slots; try a different --frame-ms");
    }
    return out_path;
}

struct ChannelDecoder {
    AudioCodecFilter<Sample> *filter = nullptr;
    std::unique_ptr<AudioCodecFilter<Sample>> filter_owned;
    AudioCodecCompressor_8BitVbrDelta<Sample> compressor;

    void setup(Anog::FilterId filter_id) {
        if (filter_id == Anog::Filter_48k_4X_Polyphase) {
            filter_owned = std::make_unique<AudioCodecFilter_4X_PolyphaseFIR<Sample>>();
        } else if (filter_id == Anog::Filter_44100_12000_Polyphase) {
            filter_owned = std::make_unique<AudioCodecFilter_44100_12000_PolyphaseFIR<Sample>>();
        } else {
            throw std::runtime_error("unsupported filter_id");
        }
        filter = filter_owned.get();
        filter->reset();
    }
};

inline void decode_anog_to_channels(
    const std::string &in_path,
    Anog::Header &out_hdr,
    std::vector<std::vector<Sample>> &out_channels) {

    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open: " + in_path);
    }

    Anog::Header hdr;
    if (!Anog::read_header(in, hdr)) {
        throw std::runtime_error("invalid ANOG header");
    }
    std::vector<Anog::SeekEntry> seek;
    if (!Anog::read_seek_table(in, hdr.seek_entry_count, seek)) {
        throw std::runtime_error("failed reading seek table");
    }

    std::vector<ChannelDecoder> decoders(hdr.channels);
    for (auto &d : decoders) {
        d.setup(Anog::FilterId(hdr.filter_id));
    }

    out_channels.assign(hdr.channels, std::vector<Sample>(size_t(hdr.pcm_total_samples), Sample(0)));

    const uint32_t max_pcm_frame =
        uint32_t(llround((double(hdr.frame_ms) * double(hdr.pcm_sample_rate)) / 1000.0)) + 8;

    for (uint32_t fi = 0; fi < hdr.seek_entry_count; fi++) {
        const uint64_t pcm_start =
            Anog::pcm_index_at_frame(fi, hdr.frame_ms, hdr.pcm_sample_rate, hdr.pcm_total_samples);
        const uint64_t pcm_end =
            Anog::pcm_index_at_frame(fi + 1, hdr.frame_ms, hdr.pcm_sample_rate, hdr.pcm_total_samples);
        const uint32_t n_pcm = uint32_t(pcm_end - pcm_start);
        if (n_pcm == 0) {
            continue;
        }

        const uint64_t tick_start = seek[fi].tick;
        const uint64_t tick_end = (fi + 1 < hdr.seek_entry_count)
                                      ? seek[fi + 1].tick
                                      : hdr.compressed_total_samples;
        const uint32_t n_comp = uint32_t(tick_end - tick_start);

        in.seekg(std::streamoff(seek[fi].file_offset));
        if (!in) {
            throw std::runtime_error("seek failed");
        }

        for (uint8_t ch = 0; ch < hdr.channels; ch++) {
            if (n_comp == 0) {
                continue;
            }
            std::vector<Sample> mid(n_comp);
            uint32_t filled = 0;
            while (filled < n_comp) {
                uint8_t pkt_ch = 0;
                uint16_t pkt_comp = 0;
                std::vector<uint8_t> payload;
                if (!Anog::read_packet(in, pkt_ch, pkt_comp, payload)) {
                    throw std::runtime_error("failed reading packet");
                }
                if (pkt_ch != ch) {
                    throw std::runtime_error("unexpected channel order");
                }
                if (pkt_comp == 0 || filled + pkt_comp > n_comp) {
                    throw std::runtime_error("packet comp_samples out of range");
                }
                uint32_t chunk_n = pkt_comp;
                decoders[ch].compressor.decompress(payload.data(), uint32_t(payload.size()),
                                                   mid.data() + filled, &chunk_n);
                if (chunk_n != pkt_comp) {
                    throw std::runtime_error("decompress sample count mismatch");
                }
                filled += pkt_comp;
            }

            std::vector<Sample> pcm_out(n_pcm ? n_pcm : 1);
            uint32_t pcm_n = n_pcm;
            const uint32_t expand_cap = decoders[ch].filter->getExpandedSamples(n_comp);
            if (expand_cap > pcm_n) {
                pcm_out.resize(expand_cap);
                pcm_n = expand_cap;
            }
            decoders[ch].filter->expand(mid.data(), n_comp, pcm_out.data(), &pcm_n);

            const uint32_t copy_n = std::min(n_pcm, pcm_n);
            for (uint32_t i = 0; i < copy_n; i++) {
                out_channels[ch][size_t(pcm_start + i)] = pcm_out[i];
            }
        }
        (void)max_pcm_frame;
    }

    out_hdr = hdr;
}

}  // namespace AnogCodec
