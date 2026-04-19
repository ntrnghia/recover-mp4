#pragma once
/// mdat scanner — finds video and audio samples in corrupted MP4 data.

#include "reference.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace recover {

/// A sample (offset, size) in the mdat.
struct Sample {
    uint64_t offset;
    uint32_t size;
};

/// A chunk: its file offset and indices into the samples array.
struct Chunk {
    uint64_t offset;
    std::vector<uint32_t> sample_indices;
};

/// Results from scanning the mdat.
struct ScanResult {
    std::vector<Sample>   video_samples;
    std::vector<Sample>   audio_samples;
    std::vector<Chunk>    video_chunks;
    std::vector<Chunk>    audio_chunks;
    std::vector<uint32_t> sync_samples;    // 1-based sample indices of IDR frames
    std::vector<uint8_t>  slice_types;     // per-sample: 1=B, 0=non-B
    bool                  has_b_frames = false;
    uint64_t              mdat_offset = 0;
    uint64_t              mdat_size = 0;
};

/// Scan corrupted mdat for interleaved video and audio data.
ScanResult scan_mdat(const std::string& corrupted_path, const ReferenceInfo& ref);

} // namespace recover
