/// Reference MP4 parser using FFmpeg's libavformat.
///
/// Extracts SPS/PPS from avcC extradata, plus timescales and sample deltas
/// from the video and audio streams. Falls back to manual moov scanning
/// for fields FFmpeg doesn't expose directly (mvhd timescale, stsc).
/// Also extracts AAC header patterns and frame size statistics from audio data.

#include "reference.hpp"
#include "constants.hpp"
#include "mmap_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <stdexcept>
#include <string_view>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace recover {

namespace {

/// RAII wrapper for AVFormatContext.
struct FormatCtx {
    AVFormatContext* ctx = nullptr;

    ~FormatCtx() {
        if (ctx) avformat_close_input(&ctx);
    }

    FormatCtx() = default;
    FormatCtx(const FormatCtx&) = delete;
    FormatCtx& operator=(const FormatCtx&) = delete;
};

/// Parse avcC blob from extradata → SPS and PPS.
void parse_avcc(const uint8_t* data, int size, ReferenceInfo& info) {
    if (size < 8) return;

    int num_sps = data[5] & 0x1F;
    int pos = 6;

    if (num_sps > 0 && pos + 2 <= size) {
        uint16_t sps_len = read_be16(data + pos);
        pos += 2;
        if (pos + sps_len <= size) {
            info.sps.assign(data + pos, data + pos + sps_len);
            pos += sps_len;
        }
    }

    if (pos < size) {
        // int num_pps = data[pos]; // typically 1
        pos += 1;
        if (pos + 2 <= size) {
            uint16_t pps_len = read_be16(data + pos);
            pos += 2;
            if (pos + pps_len <= size) {
                info.pps.assign(data + pos, data + pos + pps_len);
            }
        }
    }
}

/// Scan moov atom manually for fields FFmpeg doesn't expose directly.
/// Extracts: mvhd timescale, audio stsc (samples_per_chunk).
void scan_moov_extras(const uint8_t* data, size_t file_size, ReferenceInfo& info) {
    // Find moov atom
    size_t pos = 0;
    const uint8_t* moov = nullptr;
    size_t moov_size = 0;

    while (pos + 8 <= file_size) {
        uint32_t box_size = read_be32(data + pos);
        if (box_size < 8) break;

        if (std::memcmp(data + pos + 4, "moov", 4) == 0) {
            moov = data + pos;
            moov_size = box_size;
            break;
        }

        // Handle extended size
        if (box_size == 1 && pos + 16 <= file_size) {
            uint64_t ext_size = read_be64(data + pos + 8);
            pos += static_cast<size_t>(ext_size);
        } else {
            pos += box_size;
        }
    }

    if (!moov || moov_size < 16) return;

    // Search for mvhd within moov
    for (size_t i = 0; i + 20 <= moov_size; ++i) {
        if (std::memcmp(moov + i, "mvhd", 4) == 0) {
            // mvhd: version(1) + flags(3) + create(4) + modify(4) + timescale(4)
            if (i + 20 <= moov_size) {
                info.mvhd_timescale = read_be32(moov + i + 16);
            }
            break;
        }
    }

    // Search for audio stsc (after 'soun' handler)
    const uint8_t* soun_ptr = nullptr;
    for (size_t i = 0; i + 4 <= moov_size; ++i) {
        if (std::memcmp(moov + i, "soun", 4) == 0) {
            soun_ptr = moov + i;
            break;
        }
    }
    if (soun_ptr) {
        size_t soun_off = soun_ptr - moov;
        for (size_t i = soun_off; i + 24 <= moov_size; ++i) {
            if (std::memcmp(moov + i, "stsc", 4) == 0) {
                // full box: version+flags(4) + entry_count(4) + [first_chunk(4), spc(4), sdi(4)]*
                uint32_t entry_count = read_be32(moov + i + 8);
                if (entry_count > 0 && i + 24 <= moov_size) {
                    info.samples_per_chunk = read_be32(moov + i + 16);
                }
                break;
            }
        }
    }
}

/// Extract audio frame size stats and AAC header patterns from the reference moov + file data.
void extract_audio_patterns(const uint8_t* file_data, size_t file_size,
                            const uint8_t* moov, size_t moov_size,
                            ReferenceInfo& info) {
    // Find audio track (soun handler)
    const uint8_t* soun_ptr = nullptr;
    for (size_t i = 0; i + 4 <= moov_size; ++i) {
        if (std::memcmp(moov + i, "soun", 4) == 0) {
            soun_ptr = moov + i;
            break;
        }
    }
    if (!soun_ptr) return;
    size_t soun_off = soun_ptr - moov;

    // Get audio stsz
    const uint8_t* stsz_ptr = nullptr;
    for (size_t i = soun_off; i + 16 <= moov_size; ++i) {
        if (std::memcmp(moov + i, "stsz", 4) == 0) {
            stsz_ptr = moov + i;
            break;
        }
    }
    if (!stsz_ptr) return;

    uint32_t uniform = read_be32(stsz_ptr + 8);
    uint32_t count = read_be32(stsz_ptr + 12);
    if (count == 0) return;

    std::vector<uint32_t> sizes(count);
    if (uniform > 0) {
        std::fill(sizes.begin(), sizes.end(), uniform);
    } else {
        size_t stsz_data_off = (stsz_ptr + 16) - moov;
        if (stsz_data_off + count * 4 > moov_size) return;
        for (uint32_t i = 0; i < count; ++i) {
            sizes[i] = read_be32(stsz_ptr + 16 + i * 4);
        }
    }

    // Compute frame size stats
    double sum = 0, sum_sq = 0;
    uint32_t fmin = UINT32_MAX, fmax = 0;
    for (auto s : sizes) {
        sum += s;
        sum_sq += static_cast<double>(s) * s;
        fmin = std::min(fmin, s);
        fmax = std::max(fmax, s);
    }
    double mean = sum / count;
    double variance = sum_sq / count - mean * mean;
    double stdev = std::sqrt(std::max(0.0, variance));

    info.audio_ref.frame_size_min = static_cast<double>(fmin);
    info.audio_ref.frame_size_max = static_cast<double>(fmax);
    info.audio_ref.frame_size_mean = mean;
    info.audio_ref.frame_size_stdev = stdev;

    // Get audio stco offsets
    const uint8_t* stco_ptr = nullptr;
    for (size_t i = soun_off; i + 12 <= moov_size; ++i) {
        if (std::memcmp(moov + i, "stco", 4) == 0) {
            stco_ptr = moov + i;
            break;
        }
    }
    if (!stco_ptr) return;

    uint32_t chunk_count = read_be32(stco_ptr + 8);
    if (chunk_count == 0) return;
    size_t stco_data_off = (stco_ptr + 12) - moov;
    if (stco_data_off + chunk_count * 4 > moov_size) return;

    std::vector<uint32_t> chunk_offsets(chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        chunk_offsets[i] = read_be32(stco_ptr + 12 + i * 4);
    }

    uint32_t spc = info.samples_per_chunk;

    // Read first 3 bytes of each frame, tally header patterns
    std::map<int, int> msf_long_counts, msf_short_counts, ms_counts;
    uint32_t si = 0;
    for (auto co : chunk_offsets) {
        uint32_t n = std::min(spc, count - si);
        uint64_t off = co;
        for (uint32_t j = 0; j < n; ++j) {
            if (off + 3 <= file_size) {
                uint8_t b0 = file_data[off];
                uint8_t b1 = file_data[off + 1];
                uint8_t b2 = file_data[off + 2];
                if ((b0 == 0x20 || b0 == 0x21) && !(b1 & 0x80)) {
                    int ws = (b1 >> 5) & 0x03;
                    if (ws == 2) {
                        int msf = b1 & 0x0F;
                        msf_short_counts[msf]++;
                    } else {
                        int msf = ((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03);
                        msf_long_counts[msf]++;
                    }
                    if (b0 == 0x21 && ws != 2) {
                        int ms = (b2 >> 3) & 0x03;
                        ms_counts[ms]++;
                    }
                }
            }
            off += sizes[si];
            ++si;
        }
    }

    // Store dominant values
    auto max_key = [](const std::map<int, int>& m) -> int {
        int best_k = 0, best_v = 0;
        for (auto& [k, v] : m) {
            if (v > best_v) { best_k = k; best_v = v; }
        }
        return best_k;
    };
    if (!msf_long_counts.empty())
        info.audio_ref.dominant_msf_long = max_key(msf_long_counts);
    if (!msf_short_counts.empty())
        info.audio_ref.dominant_msf_short = max_key(msf_short_counts);
    if (!ms_counts.empty())
        info.audio_ref.dominant_ms = max_key(ms_counts);
}

} // anonymous namespace

ReferenceInfo parse_reference(const std::string& path) {
    ReferenceInfo info;

    // Phase 1: Use FFmpeg to extract streams, codec params, SPS/PPS
    FormatCtx fmt;
    int ret = avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0)
        throw std::runtime_error("Cannot open reference file: " + path);

    ret = avformat_find_stream_info(fmt.ctx, nullptr);
    if (ret < 0)
        throw std::runtime_error("Cannot find stream info: " + path);

    for (unsigned i = 0; i < fmt.ctx->nb_streams; ++i) {
        AVStream* stream = fmt.ctx->streams[i];
        AVCodecParameters* par = stream->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id == AV_CODEC_ID_H264) {
            info.width = static_cast<uint16_t>(par->width);
            info.height = static_cast<uint16_t>(par->height);

            // Timescale from stream time_base (inverted)
            if (stream->time_base.den > 0) {
                info.video_timescale = static_cast<uint32_t>(stream->time_base.den);
            }

            // Sample delta from avg_frame_rate or codec framerate
            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                // delta = timescale / fps
                double fps = static_cast<double>(stream->avg_frame_rate.num)
                            / stream->avg_frame_rate.den;
                if (fps > 0) {
                    info.video_sample_delta = static_cast<uint32_t>(
                        info.video_timescale / fps + 0.5);
                }
            }

            // Parse avcC extradata for SPS/PPS
            if (par->extradata && par->extradata_size > 0) {
                parse_avcc(par->extradata, par->extradata_size, info);
            }

        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (stream->time_base.den > 0) {
                info.audio_timescale = static_cast<uint32_t>(stream->time_base.den);
            }
            // AAC-LC frame size is always 1024 samples
            info.audio_sample_delta = 1024;
        }
    }

    // Phase 2: Manual moov scan for mvhd timescale, stsc, and AAC patterns
    try {
        MappedFile mf(path);
        // Find moov offset
        const uint8_t* moov_data = nullptr;
        size_t moov_size_val = 0;
        size_t pos = 0;
        while (pos + 8 <= mf.size()) {
            uint32_t box_size = read_be32(mf.data() + pos);
            if (box_size < 8) break;
            if (std::memcmp(mf.data() + pos + 4, "moov", 4) == 0) {
                moov_data = mf.data() + pos;
                moov_size_val = box_size;
                break;
            }
            if (box_size == 1 && pos + 16 <= mf.size()) {
                uint64_t ext_size = read_be64(mf.data() + pos + 8);
                pos += static_cast<size_t>(ext_size);
            } else {
                pos += box_size;
            }
        }
        if (moov_data && moov_size_val >= 16) {
            scan_moov_extras(moov_data, moov_size_val, info);
            extract_audio_patterns(mf.data(), mf.size(), moov_data, moov_size_val, info);
        }
    } catch (...) {
        // Non-critical — defaults are fine
    }

    return info;
}

// ─── Reference-free mode ────────────────────────────────────────────────────

namespace {

/// Bitstream writer for constructing H.264 NAL units.
class BitWriter {
    std::vector<uint8_t> data_;
    uint8_t byte_ = 0;
    int bit_pos_ = 7;

public:
    void write_u(uint32_t val, int n) {
        for (int i = n - 1; i >= 0; --i) {
            byte_ |= static_cast<uint8_t>(((val >> i) & 1) << bit_pos_);
            if (--bit_pos_ < 0) {
                data_.push_back(byte_);
                byte_ = 0;
                bit_pos_ = 7;
            }
        }
    }

    void write_ue(uint32_t val) {
        val += 1;
        int nbits = 0;
        for (uint32_t v = val; v; v >>= 1) ++nbits;
        write_u(0, nbits - 1);
        write_u(val, nbits);
    }

    std::vector<uint8_t> flush() {
        if (bit_pos_ < 7) {
            byte_ |= static_cast<uint8_t>(1 << bit_pos_);
            data_.push_back(byte_);
        }
        return data_;
    }
};

/// Insert emulation prevention bytes (0x03) per H.264 spec.
std::vector<uint8_t> add_emulation_prevention(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + rbsp.size() / 64);
    int count = 0;
    for (uint8_t b : rbsp) {
        if (count >= 2 && b <= 3) {
            out.push_back(3);
            count = 0;
        }
        out.push_back(b);
        count = (b == 0) ? count + 1 : 0;
    }
    return out;
}

/// Construct SPS NAL for Microsoft H.264 Encoder V1.5.3.
std::vector<uint8_t> build_sps(int w, int h, bool cabac) {
    int ref = cabac ? 1 : 2;

    BitWriter bw;

    // NAL header
    bw.write_u(0, 1);   // forbidden_zero_bit
    bw.write_u(3, 2);   // nal_ref_idc = 3
    bw.write_u(7, 5);   // nal_unit_type = 7 (SPS)

    bw.write_u(77, 8);  // profile_idc = Main
    bw.write_u(0, 1);   // constraint_set0
    bw.write_u(1, 1);   // constraint_set1
    bw.write_u(0, 1);   // constraint_set2
    bw.write_u(0, 1);   // constraint_set3
    bw.write_u(0, 4);   // reserved

    int w_mbs = (w + 15) / 16;
    int h_mbs = (h + 15) / 16;
    int mb_count = w_mbs * h_mbs;
    int level = (mb_count * 30 > 245760) ? 50 : 40;
    bw.write_u(static_cast<uint32_t>(level), 8);

    bw.write_ue(0);      // seq_parameter_set_id

    bw.write_ue(4);      // log2_max_frame_num_minus4

    if (cabac) {
        bw.write_ue(2);  // pic_order_cnt_type = 2
    } else {
        bw.write_ue(0);  // pic_order_cnt_type = 0
        bw.write_ue(4);  // log2_max_pic_order_cnt_lsb_minus4
    }

    bw.write_ue(static_cast<uint32_t>(ref));
    bw.write_u(0, 1);    // gaps_in_frame_num_allowed

    bw.write_ue(static_cast<uint32_t>(w_mbs - 1));
    bw.write_ue(static_cast<uint32_t>(h_mbs - 1));

    bw.write_u(1, 1);    // frame_mbs_only_flag
    bw.write_u(1, 1);    // direct_8x8_inference_flag

    // Cropping
    int crop_right = w_mbs * 16 - w;
    int crop_bottom = h_mbs * 16 - h;
    if (crop_right > 0 || crop_bottom > 0) {
        bw.write_u(1, 1);
        bw.write_ue(0);
        bw.write_ue(static_cast<uint32_t>(crop_right / 2));
        bw.write_ue(0);
        bw.write_ue(static_cast<uint32_t>(crop_bottom / 2));
    } else {
        bw.write_u(0, 1);
    }

    // VUI
    bw.write_u(1, 1);    // vui_parameters_present
    bw.write_u(1, 1);    // aspect_ratio_info_present
    bw.write_u(1, 8);    // aspect_ratio_idc = 1 (square)
    bw.write_u(0, 1);    // overscan_info_present
    bw.write_u(0, 1);    // video_signal_type_present
    bw.write_u(0, 1);    // chroma_loc_info_present
    bw.write_u(1, 1);    // timing_info_present
    if (cabac) {
        bw.write_u(1000, 32);
        bw.write_u(60000, 32);
    } else {
        bw.write_u(1, 32);
        bw.write_u(60, 32);
    }
    bw.write_u(0, 1);    // fixed_frame_rate_flag
    bw.write_u(0, 1);    // nal_hrd_parameters_present
    bw.write_u(0, 1);    // vcl_hrd_parameters_present
    bw.write_u(0, 1);    // pic_struct_present
    bw.write_u(1, 1);    // bitstream_restriction_flag
    bw.write_u(1, 1);    // motion_vectors_over_pic_boundaries
    if (cabac) {
        bw.write_ue(0);   // max_bytes_per_pic_denom
        bw.write_ue(0);   // max_bits_per_mb_denom
        bw.write_ue(13);  // log2_max_mv_length_horizontal
        bw.write_ue(9);   // log2_max_mv_length_vertical
        bw.write_ue(0);   // max_num_reorder_frames
        bw.write_ue(1);   // max_dec_frame_buffering
    } else {
        bw.write_ue(2);   // max_bytes_per_pic_denom
        bw.write_ue(1);   // max_bits_per_mb_denom
        bw.write_ue(16);  // log2_max_mv_length_horizontal
        bw.write_ue(16);  // log2_max_mv_length_vertical
        bw.write_ue(1);   // max_num_reorder_frames
        bw.write_ue(2);   // max_dec_frame_buffering
    }

    auto rbsp = bw.flush();
    return add_emulation_prevention(rbsp);
}

/// Return PPS NAL bytes for the given encoder variant.
std::vector<uint8_t> build_pps(bool cabac) {
    if (cabac) return {0x68, 0xEE, 0x3C, 0x80};
    return {0x68, 0xCE, 0x3C, 0x80};
}

/// SEI parameter string parsed from mdat.
struct SeiParams {
    int w = 0;
    int h = 0;
    int cabac = 0;
};

/// Parse 'key:value' pairs from the encoder SEI parameter string.
std::optional<SeiParams> parse_param_string(const char* text, size_t len) {
    SeiParams p;
    std::string_view sv(text, len);

    auto get_val = [&](std::string_view key) -> std::optional<int> {
        auto pos = sv.find(key);
        if (pos == std::string_view::npos) return std::nullopt;
        pos += key.size();
        int val = 0;
        while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') {
            val = val * 10 + (sv[pos] - '0');
            ++pos;
        }
        return val;
    };

    auto w_val = get_val("w:");
    auto h_val = get_val("h:");
    if (!w_val || !h_val || *w_val == 0 || *h_val == 0) return std::nullopt;

    p.w = *w_val;
    p.h = *h_val;
    p.cabac = get_val("cabac:").value_or(0);
    return p;
}

/// Parse SEI NAL payloads looking for encoder parameter string.
std::optional<SeiParams> parse_sei_payload(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos + 1 < len) {
        // Payload type
        uint32_t pt = 0;
        while (pos < len && data[pos] == 0xFF) { pt += 255; ++pos; }
        if (pos >= len) break;
        pt += data[pos++];

        // Payload size
        uint32_t ps = 0;
        while (pos < len && data[pos] == 0xFF) { ps += 255; ++pos; }
        if (pos >= len) break;
        ps += data[pos++];

        if (pt == 5 && ps >= 20 && pos + ps <= len) {
            // user_data_unregistered: 16-byte UUID + text
            const char* text = reinterpret_cast<const char*>(data + pos + 16);
            size_t text_len = ps - 16;
            // Check for param string markers
            auto sv = std::string_view(text, text_len);
            if (sv.find("w:") != std::string_view::npos &&
                sv.find("h:") != std::string_view::npos) {
                return parse_param_string(text, text_len);
            }
        }
        pos += ps;
    }
    return std::nullopt;
}

/// Parse SEI from the start of mdat data.
std::optional<SeiParams> parse_sei_from_mdat(const uint8_t* data, size_t len) {
    // Find AUD pattern
    constexpr uint8_t aud[] = {0x00, 0x00, 0x00, 0x02, 0x09};
    const uint8_t* aud_pos = nullptr;
    for (size_t i = 0; i + 5 <= len; ++i) {
        if (std::memcmp(data + i, aud, 5) == 0) {
            aud_pos = data + i;
            break;
        }
    }
    if (!aud_pos) return std::nullopt;

    size_t pos = static_cast<size_t>(aud_pos - data);
    while (pos + 9 < len) {
        uint32_t nal_len = read_be32(data + pos);
        if (nal_len < 1 || nal_len > 4096 || pos + 4 + nal_len > len) break;
        uint8_t nal_type = data[pos + 4] & 0x1F;

        if (nal_type == 9) { // AUD — skip
            pos += 4 + nal_len;
            continue;
        }
        if (nal_type == 6) { // SEI
            auto result = parse_sei_payload(data + pos + 5, nal_len - 1);
            if (result) return result;
            pos += 4 + nal_len;
            continue;
        }
        break; // slice NAL — stop
    }
    return std::nullopt;
}

} // anonymous namespace

ReferenceInfo detect_config(const std::string& path) {
    // Read mdat header to find data start, then first 4KB
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + path);

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();
    f.seekg(0);

    // Locate mdat atom
    int64_t mdat_offset = -1;
    while (f.tellg() < file_size) {
        auto pos = f.tellg();
        uint8_t hdr[16];
        f.read(reinterpret_cast<char*>(hdr), 8);
        if (!f) break;
        uint64_t size = read_be32(hdr);
        if (size == 1) {
            f.read(reinterpret_cast<char*>(hdr + 8), 8);
            if (!f) break;
            size = read_be64(hdr + 8);
        } else if (size == 0) {
            size = static_cast<uint64_t>(file_size) - static_cast<uint64_t>(pos);
        }
        if (std::memcmp(hdr + 4, "mdat", 4) == 0) {
            mdat_offset = pos;
            break;
        }
        if (size < 8) break;
        f.seekg(pos + static_cast<std::streamoff>(size));
    }

    if (mdat_offset < 0) throw std::runtime_error("No mdat atom found");

    // Read first 4KB of mdat data
    f.seekg(mdat_offset + 8);
    uint8_t head[4096];
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    auto bytes_read = f.gcount();

    auto params = parse_sei_from_mdat(head, static_cast<size_t>(bytes_read));
    if (!params) {
        throw std::runtime_error(
            "No encoder SEI found in mdat. Use a reference file or provide resolution.");
    }

    std::println("  SEI detected: {}x{}, cabac={}", params->w, params->h, params->cabac);

    bool cabac = params->cabac != 0;
    ReferenceInfo info;
    info.width = static_cast<uint16_t>(params->w);
    info.height = static_cast<uint16_t>(params->h);
    info.video_timescale = 30000;
    info.video_sample_delta = 1000;
    info.audio_timescale = 48000;
    info.audio_sample_delta = 1024;
    info.samples_per_chunk = 15;
    info.mvhd_timescale = 10'000'000;
    info.sps = build_sps(params->w, params->h, cabac);
    info.pps = build_pps(cabac);
    return info;
}

} // namespace recover
