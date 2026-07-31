#include "anog_codec_io.hpp"
#include "AudioFile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// Codec library still uses printf() for lab diagnostics; keep the CLI quiet.
class SuppressStdout {
    int saved_{-1};

public:
    SuppressStdout() {
        fflush(stdout);
        saved_ = dup(STDOUT_FILENO);
        if (saved_ < 0) {
            return;
        }
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
    }
    ~SuppressStdout() {
        fflush(stdout);
        if (saved_ >= 0) {
            dup2(saved_, STDOUT_FILENO);
            close(saved_);
        }
    }
    SuppressStdout(const SuppressStdout &) = delete;
    SuppressStdout &operator=(const SuppressStdout &) = delete;
};

static void usage(const char *argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " -enc <input.wav|*> [output.anog] [--frame-ms N]\n"
        << "  " << argv0 << " -dec <input.anog|*> [output.wav]\n"
        << "  " << argv0 << " -all [-r] [--frame-ms N]\n"
        << "\n"
        << "  -enc / -dec   Encode WAV→ANOG or decode ANOG→WAV\n"
        << "  *             Convert all matching files in the current directory\n"
        << "                (quote it for the shell: '*')\n"
        << "  -all          Encode all WAV files in cwd into ./ANOG/\n"
        << "                (-enc is optional with -all)\n"
        << "  -r            With -all: recurse; mirror folder tree under ./ANOG/\n"
        << "  --frame-ms N  Encode frame length in ms (default 100)\n"
        << "\n"
        << "Existing outputs prompt for overwrite confirmation.\n";
}

static std::string to_lower(std::string s) {
    for (char &c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool has_ext(const fs::path &p, const char *ext_lower) {
    return to_lower(p.extension().string()) == ext_lower;
}

static bool is_wav(const fs::path &p) { return has_ext(p, ".wav"); }
static bool is_anog(const fs::path &p) { return has_ext(p, ".anog"); }

static bool confirm_overwrite(const fs::path &path) {
    if (!fs::exists(path)) {
        return true;
    }
    std::cout << "Overwrite " << path.string() << "? [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

static bool ensure_parent_dirs(const fs::path &file_path) {
    const fs::path parent = file_path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(parent, ec);
    return !ec;
}

static int encode_one(const fs::path &in_path, const fs::path &out_path, uint16_t frame_ms) {
    if (!confirm_overwrite(out_path)) {
        std::cout << "Skipped " << out_path.string() << "\n";
        return 0;
    }
    if (!ensure_parent_dirs(out_path)) {
        std::cerr << "Failed to create directories for " << out_path.string() << "\n";
        return 1;
    }

    AudioFile<AnogCodec::Sample> wav;
    if (!wav.load(in_path.string())) {
        std::cerr << "Failed to load WAV: " << in_path.string() << "\n";
        return 1;
    }
    const int rate = wav.getSampleRate();
    const int nch = wav.getNumChannels();
    const int ns = wav.getNumSamplesPerChannel();
    if (nch < 1) {
        std::cerr << "WAV has no channels: " << in_path.string() << "\n";
        return 1;
    }

    std::vector<std::vector<AnogCodec::Sample>> channels;
    channels.resize(size_t(nch));
    for (int c = 0; c < nch; c++) {
        channels[size_t(c)].resize(size_t(ns));
        for (int i = 0; i < ns; i++) {
            channels[size_t(c)][size_t(i)] = wav.samples[size_t(c)][size_t(i)];
        }
    }

    try {
        SuppressStdout quiet;
        AnogCodec::encode_wav_channels_to_anog(channels, uint32_t(rate), frame_ms, out_path.string());
    } catch (const std::exception &ex) {
        std::cerr << "Encode failed (" << in_path.string() << "): " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Converted " << in_path.string() << " -> " << out_path.string()
              << " (" << nch << " ch, " << rate << " Hz, frame_ms=" << frame_ms << ")\n";
    return 0;
}

static int decode_one(const fs::path &in_path, const fs::path &out_path) {
    if (!confirm_overwrite(out_path)) {
        std::cout << "Skipped " << out_path.string() << "\n";
        return 0;
    }
    if (!ensure_parent_dirs(out_path)) {
        std::cerr << "Failed to create directories for " << out_path.string() << "\n";
        return 1;
    }

    try {
        Anog::Header hdr;
        std::vector<std::vector<AnogCodec::Sample>> channels;
        {
            SuppressStdout quiet;
            AnogCodec::decode_anog_to_channels(in_path.string(), hdr, channels);
        }

        AudioFile<AnogCodec::Sample> wav;
        wav.setSampleRate(int(hdr.pcm_sample_rate));
        wav.setBitDepth(16);
        wav.setNumChannels(int(hdr.channels));
        wav.setNumSamplesPerChannel(int(hdr.pcm_total_samples));
        for (uint8_t c = 0; c < hdr.channels; c++) {
            for (uint64_t i = 0; i < hdr.pcm_total_samples; i++) {
                wav.samples[c][size_t(i)] = channels[c][size_t(i)];
            }
        }
        if (!wav.save(out_path.string())) {
            std::cerr << "Failed to save WAV: " << out_path.string() << "\n";
            return 1;
        }
        std::cout << "Converted " << in_path.string() << " -> " << out_path.string()
                  << " (" << int(hdr.channels) << " ch, " << hdr.pcm_sample_rate << " Hz)\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "Decode failed (" << in_path.string() << "): " << ex.what() << "\n";
        return 1;
    }
}

static std::vector<fs::path> list_cwd_files_with_ext(bool want_wav) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fs::current_path(), ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path p = entry.path();
        if (want_wav ? is_wav(p) : is_anog(p)) {
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool path_has_anog_component(const fs::path &p) {
    for (const auto &part : p) {
        if (part == "ANOG") {
            return true;
        }
    }
    return false;
}

static std::vector<fs::path> collect_wavs_for_all(bool recursive) {
    std::vector<fs::path> out;
    const fs::path cwd = fs::current_path();
    std::error_code ec;

    if (!recursive) {
        for (const auto &entry : fs::directory_iterator(cwd, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            if (is_wav(entry.path())) {
                out.push_back(entry.path());
            }
        }
    } else {
        for (auto it = fs::recursive_directory_iterator(cwd, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            // Skip anything inside an ANOG output tree
            const fs::path rel = fs::relative(it->path(), cwd, ec);
            if (!ec && path_has_anog_component(rel)) {
                if (it->is_directory()) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file()) {
                continue;
            }
            if (is_wav(it->path())) {
                out.push_back(it->path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int main(int argc, char **argv) {
    bool want_enc = false;
    bool want_dec = false;
    bool want_all = false;
    bool want_recursive = false;
    uint16_t frame_ms = 100;
    std::string in_arg;
    std::string out_arg;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (std::strcmp(a, "-enc") == 0) {
            want_enc = true;
        } else if (std::strcmp(a, "-dec") == 0) {
            want_dec = true;
        } else if (std::strcmp(a, "-all") == 0) {
            want_all = true;
        } else if (std::strcmp(a, "-r") == 0) {
            want_recursive = true;
        } else if (std::strcmp(a, "--frame-ms") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            frame_ms = uint16_t(std::stoi(argv[++i]));
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && std::strcmp(a, "*") != 0) {
            std::cerr << "Unknown option: " << a << "\n";
            usage(argv[0]);
            return 2;
        } else if (in_arg.empty()) {
            in_arg = a;
        } else if (out_arg.empty()) {
            out_arg = a;
        } else {
            std::cerr << "Unexpected argument: " << a << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (want_all) {
        if (want_dec) {
            std::cerr << "-all is encode-only (do not combine with -dec)\n";
            return 2;
        }
        // -enc optional with -all
        want_enc = true;
        if (!in_arg.empty() || !out_arg.empty()) {
            std::cerr << "-all does not take input/output paths\n";
            return 2;
        }

        const fs::path anog_root = fs::current_path() / "ANOG";
        std::error_code ec;
        fs::create_directories(anog_root, ec);
        if (ec) {
            std::cerr << "Failed to create ANOG folder: " << ec.message() << "\n";
            return 1;
        }
        std::cout << "Output folder: " << anog_root.string() << "\n";

        const auto wavs = collect_wavs_for_all(want_recursive);
        if (wavs.empty()) {
            std::cout << "No WAV files found.\n";
            return 0;
        }

        int failures = 0;
        const fs::path cwd = fs::current_path();
        for (const fs::path &wav : wavs) {
            fs::path rel = fs::relative(wav, cwd, ec);
            if (ec) {
                rel = wav.filename();
            }
            fs::path out = anog_root / rel;
            out.replace_extension(".anog");
            if (encode_one(wav, out, frame_ms)) {
                failures++;
            }
        }
        return failures ? 1 : 0;
    }

    if (want_enc == want_dec) {
        std::cerr << "Specify exactly one of -enc or -dec (or use -all to encode)\n";
        usage(argv[0]);
        return 2;
    }
    if (want_recursive) {
        std::cerr << "-r is only valid with -all\n";
        return 2;
    }
    if (in_arg.empty()) {
        usage(argv[0]);
        return 2;
    }

    const bool star = (in_arg == "*");

    if (want_enc) {
        if (star) {
            if (!out_arg.empty()) {
                std::cerr << "Do not pass an output path with '*'\n";
                return 2;
            }
            const auto wavs = list_cwd_files_with_ext(true);
            if (wavs.empty()) {
                std::cout << "No WAV files in current directory.\n";
                return 0;
            }
            int failures = 0;
            for (const fs::path &wav : wavs) {
                fs::path out = wav;
                out.replace_extension(".anog");
                if (encode_one(wav, out, frame_ms)) {
                failures++;
            }
            }
            return failures ? 1 : 0;
        }

        fs::path in_path(in_arg);
        fs::path out_path = out_arg.empty() ? fs::path(in_arg).replace_extension(".anog")
                                            : fs::path(out_arg);
        return encode_one(in_path, out_path, frame_ms);
    }

    // decode
    if (star) {
        if (!out_arg.empty()) {
            std::cerr << "Do not pass an output path with '*'\n";
            return 2;
        }
        const auto anogs = list_cwd_files_with_ext(false);
        if (anogs.empty()) {
            std::cout << "No ANOG files in current directory.\n";
            return 0;
        }
        int failures = 0;
        for (const fs::path &anog : anogs) {
            fs::path out = anog;
            out.replace_extension(".wav");
            if (decode_one(anog, out)) {
                failures++;
            }
        }
        return failures ? 1 : 0;
    }

    fs::path in_path(in_arg);
    fs::path out_path = out_arg.empty() ? fs::path(in_arg).replace_extension(".wav")
                                        : fs::path(out_arg);
    return decode_one(in_path, out_path);
}
