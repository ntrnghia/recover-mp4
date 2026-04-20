#pragma once
/// Reference MP4 parser — extracts codec config and timing parameters.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recover {

/// AAC header pattern statistics from reference audio frames.
struct AudioRef {
    double frame_size_min   = 50.0;
    double frame_size_max   = 2000.0;
    double frame_size_mean  = 0.0;
    double frame_size_stdev = 0.0;

    std::optional<int> dominant_msf_long;
    std::optional<int> dominant_msf_short;
    std::optional<int> dominant_ms;
};

/// Codec and timing info extracted from a reference MP4.
struct ReferenceInfo {
    // Video
    uint32_t video_timescale   = 30000;
    uint32_t video_sample_delta = 1000;
    uint16_t width             = 1920;
    uint16_t height            = 1080;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    // Audio
    uint32_t audio_timescale   = 48000;
    uint32_t audio_sample_delta = 1024;
    uint32_t samples_per_chunk = 15;

    // Audio reference patterns
    AudioRef audio_ref;

    // Movie
    uint32_t mvhd_timescale    = 10'000'000;
};

/// Parse a reference MP4 to extract codec config and timing.
/// Uses FFmpeg's libavformat for robust parsing.
ReferenceInfo parse_reference(const std::string& path);

/// Auto-detect codec config from a corrupted file's mdat (no reference needed).
/// Parses the encoder SEI embedded by Microsoft H.264 Encoder V1.5.3.
ReferenceInfo detect_config(const std::string& path);

} // namespace recover
