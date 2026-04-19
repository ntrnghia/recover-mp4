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
#include <map>
#include <print>
#include <stdexcept>

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

} // namespace recover
