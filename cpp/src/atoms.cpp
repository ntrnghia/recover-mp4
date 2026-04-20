/// MP4 atom/box builders — constructs the moov atom and all sub-boxes.
///
/// All data is written big-endian into pre-allocated vectors.

#include "atoms.hpp"
#include "constants.hpp"

#include <algorithm>
#include <cstring>

namespace recover {

namespace {

// ─── Helpers ────────────────────────────────────────────────────────────────

/// Append bytes to output vector.
inline void append(std::vector<uint8_t>& out, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

/// Append n zero bytes.
inline void append_zeros(std::vector<uint8_t>& out, size_t n) {
    out.insert(out.end(), n, 0);
}

/// Append a BE32 value.
inline void append_be32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t buf[4];
    write_be32(buf, v);
    out.insert(out.end(), buf, buf + 4);
}

/// Append a BE16 value.
inline void append_be16(std::vector<uint8_t>& out, uint16_t v) {
    uint8_t buf[2];
    write_be16(buf, v);
    out.insert(out.end(), buf, buf + 2);
}

/// Append a BE64 value.
inline void append_be64(std::vector<uint8_t>& out, uint64_t v) {
    uint8_t buf[8];
    write_be64(buf, v);
    out.insert(out.end(), buf, buf + 8);
}

/// Append a single byte.
inline void append_byte(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

/// Write a box header at a specific position (for backpatching size).
/// Returns the position after the header (content start).
size_t begin_box(std::vector<uint8_t>& out, const char* type) {
    size_t start = out.size();
    append_be32(out, 0); // placeholder for size
    append(out, type, 4);
    return start;
}

/// Backpatch the box size.
void end_box(std::vector<uint8_t>& out, size_t start) {
    uint32_t size = static_cast<uint32_t>(out.size() - start);
    write_be32(out.data() + start, size);
}

/// Begin a full box (with version and flags).
size_t begin_full_box(std::vector<uint8_t>& out, const char* type,
                      uint8_t version, uint32_t flags) {
    size_t start = begin_box(out, type);
    append_be32(out, (static_cast<uint32_t>(version) << 24) | flags);
    return start;
}

// ─── Track-level boxes ──────────────────────────────────────────────────────

void write_mvhd(std::vector<uint8_t>& out, uint32_t timescale, uint64_t duration) {
    uint8_t version = (duration > 0xFFFFFFFFu) ? 1 : 0;
    size_t start = begin_full_box(out, "mvhd", version, 0);
    if (version == 1) {
        append_be64(out, 0); // creation_time
        append_be64(out, 0); // modification_time
        append_be32(out, timescale);
        append_be64(out, duration);
    } else {
        append_be32(out, 0);
        append_be32(out, 0);
        append_be32(out, timescale);
        append_be32(out, static_cast<uint32_t>(duration));
    }
    append_be32(out, 0x00010000); // rate = 1.0
    append_be16(out, 0x0100);    // volume = 1.0
    append_zeros(out, 10);       // reserved
    // Matrix (identity)
    for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u})
        append_be32(out, v);
    append_zeros(out, 24); // pre_defined
    append_be32(out, 3);   // next_track_ID
    end_box(out, start);
}

void write_tkhd(std::vector<uint8_t>& out, uint32_t track_id, uint64_t duration,
                uint16_t width, uint16_t height, bool is_audio) {
    uint8_t version = (duration > 0xFFFFFFFFu) ? 1 : 0;
    size_t start = begin_full_box(out, "tkhd", version, 3);
    if (version == 1) {
        append_be64(out, 0); // creation_time
        append_be64(out, 0); // modification_time
        append_be32(out, track_id);
        append_zeros(out, 4); // reserved
        append_be64(out, duration);
    } else {
        append_be32(out, 0);
        append_be32(out, 0);
        append_be32(out, track_id);
        append_zeros(out, 4);
        append_be32(out, static_cast<uint32_t>(duration));
    }
    append_zeros(out, 8); // reserved
    append_be16(out, 0);  // layer
    append_be16(out, 0);  // alternate_group
    append_be16(out, is_audio ? 0x0100 : 0); // volume
    append_zeros(out, 2); // reserved
    // Matrix
    for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u})
        append_be32(out, v);
    append_be32(out, static_cast<uint32_t>(width) << 16);
    append_be32(out, static_cast<uint32_t>(height) << 16);
    end_box(out, start);
}

void write_mdhd(std::vector<uint8_t>& out, uint32_t timescale, uint32_t duration) {
    size_t start = begin_full_box(out, "mdhd", 0, 0);
    append_be32(out, 0); // creation_time
    append_be32(out, 0); // modification_time
    append_be32(out, timescale);
    append_be32(out, duration);
    append_be16(out, 0x55C4); // language='und'
    append_be16(out, 0);      // pre_defined
    end_box(out, start);
}

void write_hdlr(std::vector<uint8_t>& out, const char* handler_type, const char* name) {
    size_t start = begin_full_box(out, "hdlr", 0, 0);
    append_zeros(out, 4); // pre_defined
    append(out, handler_type, 4);
    append_zeros(out, 12); // reserved
    size_t name_len = std::strlen(name);
    append(out, name, name_len);
    append_byte(out, 0); // null terminator
    end_box(out, start);
}

void write_vmhd(std::vector<uint8_t>& out) {
    size_t start = begin_full_box(out, "vmhd", 0, 1);
    append_zeros(out, 8); // graphicsmode + opcolor
    end_box(out, start);
}

void write_smhd(std::vector<uint8_t>& out) {
    size_t start = begin_full_box(out, "smhd", 0, 0);
    append_zeros(out, 4); // balance + reserved
    end_box(out, start);
}

void write_dinf(std::vector<uint8_t>& out) {
    size_t dinf_start = begin_box(out, "dinf");
    {
        size_t dref_start = begin_full_box(out, "dref", 0, 0);
        append_be32(out, 1); // entry_count
        {
            size_t url_start = begin_full_box(out, "url ", 0, 1);
            end_box(out, url_start);
        }
        end_box(out, dref_start);
    }
    end_box(out, dinf_start);
}

// ─── Sample description boxes ───────────────────────────────────────────────

void write_stsd_video(std::vector<uint8_t>& out,
                      const std::vector<uint8_t>& sps,
                      const std::vector<uint8_t>& pps,
                      uint16_t width, uint16_t height) {
    size_t stsd_start = begin_full_box(out, "stsd", 0, 0);
    append_be32(out, 1); // entry_count
    {
        size_t avc1_start = begin_box(out, "avc1");
        append_zeros(out, 6);     // reserved
        append_be16(out, 1);      // data_reference_index
        append_zeros(out, 16);    // pre_defined + reserved
        append_be16(out, width);
        append_be16(out, height);
        append_be32(out, 0x00480000); // 72 dpi horiz
        append_be32(out, 0x00480000); // 72 dpi vert
        append_be32(out, 0);      // data_size
        append_be16(out, 1);      // frame_count
        // compressor_name: 32 bytes, pascal string
        append_byte(out, 0x0A);
        append(out, "AVC Coding", 10);
        append_zeros(out, 21);
        append_be16(out, 0x0018); // depth=24
        append_be16(out, static_cast<uint16_t>(0xFFFF)); // color_table=-1
        {
            // avcC box
            size_t avcc_start = begin_box(out, "avcC");
            append_byte(out, 1); // configurationVersion
            if (sps.size() >= 4) {
                append(out, sps.data() + 1, 3); // profile, compat, level
            } else {
                uint8_t def[] = {0x4D, 0x40, 0x28};
                append(out, def, 3);
            }
            append_byte(out, 0xFF); // lengthSizeMinusOne = 3
            append_byte(out, 0xE1); // numSPS = 1
            append_be16(out, static_cast<uint16_t>(sps.size()));
            append(out, sps.data(), sps.size());
            append_byte(out, 1);    // numPPS = 1
            append_be16(out, static_cast<uint16_t>(pps.size()));
            append(out, pps.data(), pps.size());
            end_box(out, avcc_start);
        }
        end_box(out, avc1_start);
    }
    end_box(out, stsd_start);
}

void write_stsd_audio(std::vector<uint8_t>& out, uint32_t timescale) {
    size_t stsd_start = begin_full_box(out, "stsd", 0, 0);
    append_be32(out, 1); // entry_count
    {
        size_t mp4a_start = begin_box(out, "mp4a");
        append_zeros(out, 6);    // reserved
        append_be16(out, 1);     // data_reference_index
        append_zeros(out, 8);    // reserved
        append_be16(out, 2);     // channel_count
        append_be16(out, 16);    // sample_size
        append_zeros(out, 4);    // pre_defined + reserved
        append_be32(out, timescale << 16); // sample_rate (fixed-point 16.16)
        {
            // esds box
            size_t esds_start = begin_full_box(out, "esds", 0, 0);

            // ES_Descriptor
            auto desc_tag = [&](uint8_t tag, const std::vector<uint8_t>& content) {
                append_byte(out, tag);
                // Extended length encoding
                append_byte(out, 0x80);
                append_byte(out, 0x80);
                append_byte(out, 0x80);
                append_byte(out, static_cast<uint8_t>(content.size()));
                append(out, content.data(), content.size());
            };

            // Build from innermost out
            // DecoderSpecificInfo (tag 0x05): AAC-LC config
            std::vector<uint8_t> dec_spec;
            dec_spec.push_back(0x05);
            dec_spec.push_back(0x80); dec_spec.push_back(0x80);
            dec_spec.push_back(0x80); dec_spec.push_back(0x02);
            dec_spec.push_back(0x11); dec_spec.push_back(0x90); // AAC-LC, 48kHz, stereo

            // DecoderConfigDescriptor (tag 0x04)
            std::vector<uint8_t> dec_cfg_content;
            dec_cfg_content.push_back(0x40); // objectTypeIndication = AAC
            dec_cfg_content.push_back(0x15); // streamType = audio
            dec_cfg_content.push_back(0x00);
            dec_cfg_content.push_back(0x06);
            dec_cfg_content.push_back(0x00);
            // maxBitrate
            uint8_t br[4];
            write_be32(br, 192000);
            dec_cfg_content.insert(dec_cfg_content.end(), br, br + 4);
            // avgBitrate
            dec_cfg_content.insert(dec_cfg_content.end(), br, br + 4);
            // DecoderSpecificInfo
            dec_cfg_content.insert(dec_cfg_content.end(), dec_spec.begin(), dec_spec.end());

            std::vector<uint8_t> dec_cfg;
            dec_cfg.push_back(0x04);
            dec_cfg.push_back(0x80); dec_cfg.push_back(0x80);
            dec_cfg.push_back(0x80);
            dec_cfg.push_back(static_cast<uint8_t>(dec_cfg_content.size()));
            dec_cfg.insert(dec_cfg.end(), dec_cfg_content.begin(), dec_cfg_content.end());

            // ES_Descriptor (tag 0x03)
            std::vector<uint8_t> es_content;
            es_content.push_back(0x00); es_content.push_back(0x00); // ES_ID
            es_content.push_back(0x00); // streamPriority
            es_content.insert(es_content.end(), dec_cfg.begin(), dec_cfg.end());
            es_content.push_back(0x06); // SLConfigDescriptor tag
            es_content.push_back(0x01); // length
            es_content.push_back(0x02); // predefined = 2

            desc_tag(0x03, es_content);

            end_box(out, esds_start);
        }
        end_box(out, mp4a_start);
    }
    end_box(out, stsd_start);
}

// ─── Sample table boxes ─────────────────────────────────────────────────────

void write_stts(std::vector<uint8_t>& out, uint32_t sample_count, uint32_t sample_delta) {
    size_t start = begin_full_box(out, "stts", 0, 0);
    append_be32(out, 1); // entry_count
    append_be32(out, sample_count);
    append_be32(out, sample_delta);
    end_box(out, start);
}

void write_ctts(std::vector<uint8_t>& out, const std::vector<uint8_t>& slice_types,
                uint32_t sample_delta) {
    if (slice_types.empty()) return;

    uint32_t delta2 = 2 * sample_delta;

    // Build RLE entries
    std::vector<std::pair<uint32_t, uint32_t>> entries; // (count, offset)
    int prev_offset = -1;
    uint32_t count = 0;

    for (size_t i = 0; i < slice_types.size(); ++i) {
        uint32_t offset;
        if (slice_types[i] == 1) {
            offset = 0;
        } else if (i + 1 < slice_types.size() && slice_types[i + 1] == 1) {
            offset = delta2;
        } else {
            offset = sample_delta;
        }

        if (static_cast<int>(offset) == prev_offset) {
            ++count;
        } else {
            if (count > 0) {
                entries.emplace_back(count, static_cast<uint32_t>(prev_offset));
            }
            prev_offset = static_cast<int>(offset);
            count = 1;
        }
    }
    if (count > 0) {
        entries.emplace_back(count, static_cast<uint32_t>(prev_offset));
    }

    size_t start = begin_full_box(out, "ctts", 0, 0);
    append_be32(out, static_cast<uint32_t>(entries.size()));
    for (auto& [c, o] : entries) {
        append_be32(out, c);
        append_be32(out, o);
    }
    end_box(out, start);
}

void write_stsc(std::vector<uint8_t>& out, const std::vector<Chunk>& chunks) {
    // Compress runs with same sample count
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> entries; // (first_chunk, spc, sdi)
    uint32_t prev_n = 0;

    for (size_t i = 0; i < chunks.size(); ++i) {
        uint32_t n = static_cast<uint32_t>(chunks[i].sample_indices.size());
        if (n != prev_n) {
            entries.emplace_back(static_cast<uint32_t>(i + 1), n, 1u);
            prev_n = n;
        }
    }
    if (entries.empty()) {
        entries.emplace_back(1u, 1u, 1u);
    }

    size_t start = begin_full_box(out, "stsc", 0, 0);
    append_be32(out, static_cast<uint32_t>(entries.size()));
    for (auto& [fc, spc, di] : entries) {
        append_be32(out, fc);
        append_be32(out, spc);
        append_be32(out, di);
    }
    end_box(out, start);
}

void write_stsz(std::vector<uint8_t>& out, const std::vector<Sample>& samples) {
    size_t start = begin_full_box(out, "stsz", 0, 0);
    append_be32(out, 0); // sample_size (0 = variable)
    append_be32(out, static_cast<uint32_t>(samples.size()));
    for (const auto& s : samples) {
        append_be32(out, s.size);
    }
    end_box(out, start);
}

void write_stco(std::vector<uint8_t>& out, const std::vector<Chunk>& chunks, bool use_co64) {
    if (use_co64) {
        size_t start = begin_full_box(out, "co64", 0, 0);
        append_be32(out, static_cast<uint32_t>(chunks.size()));
        for (const auto& c : chunks) {
            uint8_t buf[8];
            write_be64(buf, c.offset);
            append(out, buf, 8);
        }
        end_box(out, start);
    } else {
        size_t start = begin_full_box(out, "stco", 0, 0);
        append_be32(out, static_cast<uint32_t>(chunks.size()));
        for (const auto& c : chunks) {
            append_be32(out, static_cast<uint32_t>(c.offset));
        }
        end_box(out, start);
    }
}

void write_stss(std::vector<uint8_t>& out, const std::vector<uint32_t>& sync_samples) {
    size_t start = begin_full_box(out, "stss", 0, 0);
    append_be32(out, static_cast<uint32_t>(sync_samples.size()));
    for (uint32_t s : sync_samples) {
        append_be32(out, s);
    }
    end_box(out, start);
}

} // anonymous namespace


std::vector<uint8_t> build_moov(const ReferenceInfo& ref, const ScanResult& scan) {
    const auto& vs = scan.video_samples;
    const auto& as = scan.audio_samples;
    const auto& vc = scan.video_chunks;
    const auto& ac = scan.audio_chunks;
    const auto& ss = scan.sync_samples;

    uint32_t mvhd_ts = ref.mvhd_timescale;
    uint32_t v_ts = ref.video_timescale;
    uint32_t a_ts = ref.audio_timescale;
    uint32_t v_delta = ref.video_sample_delta;
    uint32_t a_delta = ref.audio_sample_delta;

    uint32_t v_media_dur = static_cast<uint32_t>(vs.size()) * v_delta;
    uint32_t a_media_dur = static_cast<uint32_t>(as.size()) * a_delta;
    uint64_t v_tkhd_dur = static_cast<uint64_t>(v_media_dur) * mvhd_ts / v_ts;
    uint64_t a_tkhd_dur = as.empty() ? 0 :
        static_cast<uint64_t>(a_media_dur) * mvhd_ts / a_ts;
    uint64_t mvhd_dur = std::max(v_tkhd_dur, a_tkhd_dur);

    // Determine if we need co64
    uint64_t max_offset = 0;
    for (const auto& c : vc) max_offset = std::max(max_offset, c.offset);
    for (const auto& c : ac) max_offset = std::max(max_offset, c.offset);
    bool use_co64 = max_offset > 0xFFFFFFFFu;

    // Estimate output size (~1.2 MB for typical file)
    std::vector<uint8_t> out;
    out.reserve(2 * 1024 * 1024);

    size_t moov_start = begin_box(out, "moov");

    // mvhd
    write_mvhd(out, mvhd_ts, mvhd_dur);

    // ── Video trak ──
    {
        const auto& sps = ref.sps.empty()
            ? ([]() -> const std::vector<uint8_t>& {
                static const std::vector<uint8_t> def = {0x67, 0x4D, 0x40, 0x28};
                return def;
            })()
            : ref.sps;
        const auto& pps = ref.pps.empty()
            ? ([]() -> const std::vector<uint8_t>& {
                static const std::vector<uint8_t> def = {0x68, 0xCE, 0x3C, 0x80};
                return def;
            })()
            : ref.pps;

        size_t trak_start = begin_box(out, "trak");
        write_tkhd(out, 1, v_tkhd_dur, ref.width, ref.height, false);
        {
            size_t mdia_start = begin_box(out, "mdia");
            write_mdhd(out, v_ts, v_media_dur);
            write_hdlr(out, "vide", "VideoHandler");
            {
                size_t minf_start = begin_box(out, "minf");
                write_vmhd(out);
                write_dinf(out);
                {
                    size_t stbl_start = begin_box(out, "stbl");
                    write_stsd_video(out, sps, pps, ref.width, ref.height);
                    write_stts(out, static_cast<uint32_t>(vs.size()), v_delta);
                    write_stsc(out, vc);
                    write_stsz(out, vs);
                    write_stco(out, vc, use_co64);
                    if (scan.has_b_frames) {
                        write_ctts(out, scan.slice_types, v_delta);
                    }
                    write_stss(out, ss.empty()
                        ? std::vector<uint32_t>{1}
                        : ss);
                    end_box(out, stbl_start);
                }
                end_box(out, minf_start);
            }
            end_box(out, mdia_start);
        }
        end_box(out, trak_start);
    }

    // ── Audio trak ──
    if (!as.empty()) {
        size_t trak_start = begin_box(out, "trak");
        write_tkhd(out, 2, a_tkhd_dur, 0, 0, true);
        {
            size_t mdia_start = begin_box(out, "mdia");
            write_mdhd(out, a_ts, a_media_dur);
            write_hdlr(out, "soun", "SoundHandler");
            {
                size_t minf_start = begin_box(out, "minf");
                write_smhd(out);
                write_dinf(out);
                {
                    size_t stbl_start = begin_box(out, "stbl");
                    write_stsd_audio(out, a_ts);
                    write_stts(out, static_cast<uint32_t>(as.size()), a_delta);
                    write_stsc(out, ac);
                    write_stsz(out, as);
                    write_stco(out, ac, use_co64);
                    end_box(out, stbl_start);
                }
                end_box(out, minf_start);
            }
            end_box(out, mdia_start);
        }
        end_box(out, trak_start);
    }

    end_box(out, moov_start);
    return out;
}

} // namespace recover
