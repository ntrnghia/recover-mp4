#pragma once
/// MP4 atom/box builders — constructs the moov atom and all sub-boxes.

#include "reference.hpp"
#include "scanner.hpp"

#include <cstdint>
#include <vector>

namespace recover {

/// Build the complete moov atom from reference info and scan results.
std::vector<uint8_t> build_moov(const ReferenceInfo& ref, const ScanResult& scan);

} // namespace recover
