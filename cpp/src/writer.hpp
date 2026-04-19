#pragma once
/// Output file writer and FFmpeg-based audio fixer.

#include "scanner.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace recover {

/// Write recovered MP4: copy pre-mdat + mdat data + append moov.
void write_output(const std::string& corrupted_path, const std::string& output_path,
                  const ScanResult& scan, const std::vector<uint8_t>& moov);

/// Re-encode audio with FFmpeg C API to fix frame boundary errors.
/// Returns true on success.
bool fix_audio(const std::string& output_path);

} // namespace recover
