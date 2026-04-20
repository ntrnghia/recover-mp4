/// mdat scanner — finds video and audio samples in corrupted MP4 data.
///
/// Scans interleaved video/audio data in a corrupted Snipping Tool MP4:
///   [ftyp][uuid][mdat: [8-byte preamble][V chunk][A chunk][V chunk][A chunk]...]
///
/// Video chunks contain length-prefixed H.264 NAL units.
/// Audio chunks contain raw AAC-LC frames (no length prefix).
///
/// Uses memory-mapped I/O for zero-copy access — OS handles paging.

#include "scanner.hpp"
#include "constants.hpp"
#include "mmap_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <print>

namespace recover {

namespace {

// ─── Video chunk detection ──────────────────────────────────────────────────

/// Validate video chunk start using NAL header checks.
/// Requires AUD (length=2, type=9) followed by at least 1 valid NAL header.
bool validate_video(const uint8_t* p, size_t avail, uint64_t file_pos, uint64_t file_end) {
    size_t check_len = std::min<size_t>(64, std::min<size_t>(avail, file_end - file_pos));
    if (check_len < 11) return false;

    // First NAL must be AUD: length=2, type=9
    if (read_be32(p) != 2) return false;
    if ((p[4] & 0x1F) != 9 || (p[4] & 0x80)) return false;

    // Second NAL header at offset 6
    uint32_t nl = read_be32(p + 6);
    uint8_t nt = p[10] & 0x1F;
    if (nl < 1 || nl > MAX_NAL_SIZE ||
        !((VALID_NAL_MASK >> nt) & 1) ||
        (p[10] & 0x80))
        return false;
    if (file_pos + 10 + nl > file_end) return false;

    // Optional 3rd NAL header if 2nd NAL fits in buffer
    size_t np = 10 + nl;
    if (np + 5 <= check_len) {
        uint32_t nl2 = read_be32(p + np);
        uint8_t nt2 = p[np + 4] & 0x1F;
        if (nl2 < 1 || nl2 > MAX_NAL_SIZE ||
            !((VALID_NAL_MASK >> nt2) & 1) ||
            (p[np + 4] & 0x80))
            return false;
        if (file_pos + np + 4 + nl2 > file_end) return false;
    }

    return true;
}

/// Scan forward from start to find the next video chunk start.
/// Uses std::search for AUD pattern matching.
const uint8_t* find_next_video(const uint8_t* start, const uint8_t* end,
                                const uint8_t* file_begin, uint64_t file_end) {
    const uint8_t* pos = start;

    while (pos + AUD_PATTERN.size() <= end) {
        auto it = std::search(pos, end, AUD_PATTERN.begin(), AUD_PATTERN.end());
        if (it == end) break;

        size_t avail = static_cast<size_t>(end - it);
        uint64_t file_pos = static_cast<uint64_t>(it - file_begin);

        if (avail >= 11 && validate_video(it, avail, file_pos, file_end)) {
            return it;
        }

        pos = it + 1;
    }

    return nullptr;
}

// ─── AAC boundary detection ─────────────────────────────────────────────────

/// DP partition solver with quality-weighted cost.
/// Returns boundary positions or empty on failure.
/// Noinline: prevents GCC 15 false-positive -Wfree-nonheap-object from
/// over-aggressive inlining through find_aac_boundaries → create_audio_chunk.
[[gnu::noinline]]
std::vector<uint32_t> dp_solve(const std::vector<int>& candidates,
                                const std::vector<double>& scaled_scores,
                                int num_frames, uint32_t chunk_size,
                                double cost_mean, double inv_scale,
                                int min_size, int max_size) {
    int nc = static_cast<int>(candidates.size());
    constexpr double INF = std::numeric_limits<double>::infinity();

    std::vector<double> dp(static_cast<size_t>(num_frames) * nc, INF);
    std::vector<int> parent(static_cast<size_t>(num_frames) * nc, -1);
    dp[0] = 0.0;

    for (int fr = 0; fr < num_frames - 1; ++fr) {
        size_t fr_off = static_cast<size_t>(fr) * nc;
        size_t next_off = static_cast<size_t>(fr + 1) * nc;

        for (int ci = 0; ci < nc; ++ci) {
            double base = dp[fr_off + ci];
            if (base >= INF) continue;

            int p = candidates[ci];
            int lo_val = p + min_size;
            int hi_val = p + max_size;

            int lo = static_cast<int>(
                std::lower_bound(candidates.begin() + ci + 1, candidates.end(), lo_val)
                - candidates.begin());
            int hi = static_cast<int>(
                std::upper_bound(candidates.begin() + lo, candidates.end(), hi_val)
                - candidates.begin());

            for (int cj = lo; cj < hi; ++cj) {
                double frame_size = candidates[cj] - p;
                double z = (frame_size - cost_mean) * inv_scale;
                double total = base + z * z - scaled_scores[cj];
                if (total < dp[next_off + cj]) {
                    dp[next_off + cj] = total;
                    parent[next_off + cj] = ci;
                }
            }
        }
    }

    // Find best last-frame start
    double best_cost = INF;
    int best_ci = -1;
    size_t last_off = static_cast<size_t>(num_frames - 1) * nc;

    for (int ci = 0; ci < nc; ++ci) {
        if (dp[last_off + ci] >= INF) continue;
        int last_size = static_cast<int>(chunk_size) - candidates[ci];
        if (last_size < min_size || last_size > max_size) continue;
        double z = (last_size - cost_mean) * inv_scale;
        double total = dp[last_off + ci] + z * z;
        if (total < best_cost) {
            best_cost = total;
            best_ci = ci;
        }
    }

    if (best_ci == -1) return {};

    // Backtrack
    std::vector<int> path(num_frames);
    path[num_frames - 1] = best_ci;
    for (int fr = num_frames - 1; fr > 0; --fr) {
        path[fr - 1] = parent[static_cast<size_t>(fr) * nc + path[fr]];
    }

    std::vector<uint32_t> boundaries(num_frames + 1);
    for (int i = 0; i < num_frames; ++i) {
        boundaries[i] = static_cast<uint32_t>(candidates[path[i]]);
    }
    boundaries[num_frames] = chunk_size;

    return boundaries;
}

/// Find AAC-LC frame boundaries using quality-scored DP partition.
/// Candidates are scored based on how well they match reference AAC header patterns.
/// Tries tight reference-based bounds first, falls back to loose bounds.
std::vector<uint32_t> find_aac_boundaries(const uint8_t* chunk_data, uint32_t chunk_size,
                                           int num_frames, const AudioRef& audio_ref) {
    double target_size = static_cast<double>(chunk_size) / num_frames;

    // Build candidates with quality scores
    std::vector<int> candidates = {0};
    std::vector<double> cand_scores = {0.0};

    const uint8_t targets[] = {0x20, 0x21};

    for (uint8_t target : targets) {
        bool is_cw1 = (target == 0x21);
        for (uint32_t idx = 1; idx < chunk_size - 3; ++idx) {
            if (chunk_data[idx] != target) continue;

            uint8_t b1 = chunk_data[idx + 1];
            uint8_t b2 = chunk_data[idx + 2];

            // ics_reserved_bit must be 0
            if (b1 & 0x80) continue;

            int ws = (b1 >> 5) & 0x03;
            int msf;

            if (ws == 2) {  // EIGHT_SHORT: 4-bit max_sfb
                msf = b1 & 0x0F;
                if (msf > 14) continue;
            } else {  // Long windows: 6-bit max_sfb
                msf = ((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03);
                if (msf > 49) continue;
                if (b2 & 0x20) continue;  // predictor_data_present must be 0
            }

            // Quality scoring
            double score = 0;
            if (is_cw1) score += 3;  // common_window=1 dominant for stereo

            if (ws == 0) score += 2;       // ONLY_LONG most common
            else if (ws == 2) score -= 1;  // EIGHT_SHORT is rare

            // max_sfb matching: strongest discriminator
            if (ws == 2) {
                if (audio_ref.dominant_msf_short && msf == *audio_ref.dominant_msf_short)
                    score += 6;
            } else {
                if (audio_ref.dominant_msf_long) {
                    if (msf == *audio_ref.dominant_msf_long)
                        score += 6;
                    else if (msf == 0)
                        score += 1;  // silence frames have msf=0
                    else
                        score -= 3;  // wrong msf is strong negative
                }
            }

            // Check ms_mask_present for CW=1 long windows
            if (is_cw1 && ws != 2) {
                int ms_mask = (b2 >> 3) & 0x03;
                if (ms_mask == 3) continue;  // reserved → reject
                if (audio_ref.dominant_ms) {
                    if (ms_mask == *audio_ref.dominant_ms)
                        score += 3;
                    else
                        score -= 1;
                } else if (ms_mask <= 1) {
                    score += 1;
                }
            }

            candidates.push_back(static_cast<int>(idx));
            cand_scores.push_back(score);
        }
    }

    // Sort candidates and scores together
    if (candidates.size() > 1) {
        std::vector<size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return candidates[a] < candidates[b]; });
        std::vector<int> sorted_cands(candidates.size());
        std::vector<double> sorted_scores(candidates.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sorted_cands[i] = candidates[order[i]];
            sorted_scores[i] = cand_scores[order[i]];
        }
        // Remove duplicates (keep first occurrence)
        std::vector<int> dedup_cands;
        std::vector<double> dedup_scores;
        for (size_t i = 0; i < sorted_cands.size(); ++i) {
            if (i == 0 || sorted_cands[i] != sorted_cands[i - 1]) {
                dedup_cands.push_back(sorted_cands[i]);
                dedup_scores.push_back(sorted_scores[i]);
            }
        }
        candidates = std::move(dedup_cands);
        cand_scores = std::move(dedup_scores);
    }

    int nc = static_cast<int>(candidates.size());
    if (nc < num_frames) return {};

    // Safety cap: subsample to keep DP tractable, preserving high-quality candidates
    constexpr int MAX_CANDIDATES = 500;
    if (nc > MAX_CANDIDATES) {
        std::vector<size_t> high_q, low_q;
        for (size_t i = 1; i < static_cast<size_t>(nc); ++i) {
            if (cand_scores[i] >= 8.0)
                high_q.push_back(i);
            else
                low_q.push_back(i);
        }
        int budget = MAX_CANDIDATES - 1 - static_cast<int>(high_q.size());
        if (budget > 0 && static_cast<int>(low_q.size()) > budget) {
            double step = static_cast<double>(low_q.size()) / budget;
            std::vector<size_t> sampled_low;
            for (int i = 0; i < budget; ++i) {
                sampled_low.push_back(low_q[static_cast<int>(i * step)]);
            }
            low_q = std::move(sampled_low);
        } else if (budget <= 0) {
            low_q.clear();
        }
        std::vector<size_t> sampled = {0};
        sampled.insert(sampled.end(), high_q.begin(), high_q.end());
        sampled.insert(sampled.end(), low_q.begin(), low_q.end());
        std::sort(sampled.begin(), sampled.end());
        sampled.erase(std::unique(sampled.begin(), sampled.end()), sampled.end());

        std::vector<int> new_cands;
        std::vector<double> new_scores;
        for (auto i : sampled) {
            new_cands.push_back(candidates[i]);
            new_scores.push_back(cand_scores[i]);
        }
        candidates = std::move(new_cands);
        cand_scores = std::move(new_scores);
        nc = static_cast<int>(candidates.size());
    }

    // Use reference stats for cost function
    double cost_mean = (audio_ref.frame_size_mean > 0) ? audio_ref.frame_size_mean : target_size;
    double cost_scale = (audio_ref.frame_size_stdev > 0) ? audio_ref.frame_size_stdev
                        : std::max(1.0, target_size * 0.08);
    double inv_scale = 1.0 / cost_scale;
    double quality_weight = cost_scale * 0.6;
    std::vector<double> scaled_scores(nc);
    for (int i = 0; i < nc; ++i) {
        scaled_scores[i] = cand_scores[i] * quality_weight;
    }

    // Try tight bounds first, then loose
    struct Bounds { int min_size; int max_size; };
    std::vector<Bounds> bounds_list;
    if (audio_ref.frame_size_min > 0 && audio_ref.frame_size_max > 0) {
        bounds_list.push_back({
            std::max(1, static_cast<int>(audio_ref.frame_size_min * 0.7)),
            static_cast<int>(audio_ref.frame_size_max * 1.3)
        });
    }
    bounds_list.push_back({
        std::max(50, static_cast<int>(target_size * 0.15)),
        static_cast<int>(target_size * 3.0)
    });

    for (auto& [min_size, max_size] : bounds_list) {
        auto result = dp_solve(candidates, scaled_scores, num_frames,
                               chunk_size, cost_mean, inv_scale,
                               min_size, max_size);
        if (!result.empty()) return result;
    }

    return {};
}

/// Split an audio region into samples using AAC frame boundary detection.
void create_audio_chunk(const uint8_t* data, uint64_t start, uint64_t end,
                        uint32_t samples_per_chunk,
                        std::vector<Sample>& audio_samples,
                        std::vector<Chunk>& audio_chunks,
                        const AudioRef& audio_ref) {
    uint32_t chunk_size = static_cast<uint32_t>(end - start);
    if (chunk_size < 10) return;

    uint32_t n = samples_per_chunk;
    uint32_t min_aac_frame = static_cast<uint32_t>(
        (audio_ref.frame_size_min > 0) ? audio_ref.frame_size_min : 50);
    if (chunk_size < n * min_aac_frame) {
        n = std::max(1u, chunk_size / min_aac_frame);
    }

    std::vector<uint32_t> boundaries;
    if (n > 1) {
        double avg_frame = static_cast<double>(chunk_size) / n;
        if (avg_frame <= 2000.0) {
            boundaries = find_aac_boundaries(data + start, chunk_size,
                                             static_cast<int>(n), audio_ref);
        }
    }

    Chunk chunk;
    chunk.offset = start;

    if (!boundaries.empty() && boundaries.size() == n + 1) {
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t sz = boundaries[i + 1] - boundaries[i];
            if (sz < 1) sz = 1;
            audio_samples.push_back({start + boundaries[i], sz});
            chunk.sample_indices.push_back(
                static_cast<uint32_t>(audio_samples.size() - 1));
        }
    } else {
        // Fallback: equal splitting
        uint32_t base_size = chunk_size / n;
        uint32_t remainder = chunk_size % n;
        uint64_t offset = start;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t sz = base_size + (i < remainder ? 1 : 0);
            audio_samples.push_back({offset, sz});
            chunk.sample_indices.push_back(
                static_cast<uint32_t>(audio_samples.size() - 1));
            offset += sz;
        }
    }

    audio_chunks.push_back(std::move(chunk));
}

/// Bootstrap audio reference stats from equal-split samples.
/// Reads the first 3 bytes of each frame to extract dominant header patterns.
void bootstrap_audio_ref(const uint8_t* file_data,
                          const std::vector<Sample>& audio_samples,
                          AudioRef& audio_ref) {
    if (audio_samples.empty()) return;

    // Frame size stats
    std::vector<double> sizes;
    sizes.reserve(audio_samples.size());
    for (const auto& s : audio_samples)
        sizes.push_back(static_cast<double>(s.size));

    double sum = 0;
    for (double sz : sizes) sum += sz;
    double mean = sum / sizes.size();
    double var = 0;
    for (double sz : sizes) var += (sz - mean) * (sz - mean);
    var /= sizes.size();

    double mn = *std::min_element(sizes.begin(), sizes.end());
    double mx = *std::max_element(sizes.begin(), sizes.end());

    audio_ref.frame_size_min = mn;
    audio_ref.frame_size_max = mx;
    audio_ref.frame_size_mean = mean;
    audio_ref.frame_size_stdev = std::sqrt(var);

    // Header pattern stats
    std::map<int, int> msf_long_counts, msf_short_counts, ms_counts;

    for (const auto& s : audio_samples) {
        if (s.size < 3) continue;
        const uint8_t* hdr = file_data + s.offset;
        uint8_t b0 = hdr[0], b1 = hdr[1], b2 = hdr[2];

        if (b0 != 0x20 && b0 != 0x21) continue;
        if (b1 & 0x80) continue;  // ics_reserved_bit

        int ws = (b1 >> 5) & 0x03;
        if (ws == 2) {
            int msf = b1 & 0x0F;
            if (msf <= 14) msf_short_counts[msf]++;
        } else {
            int msf = ((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03);
            if (msf <= 49 && !(b2 & 0x20))
                msf_long_counts[msf]++;
        }
        if (b0 == 0x21 && ws != 2) {
            int ms = (b2 >> 3) & 0x03;
            if (ms != 3) ms_counts[ms]++;
        }
    }

    auto max_key = [](const std::map<int, int>& m) -> int {
        int best_k = 0, best_v = 0;
        for (auto& [k, v] : m) {
            if (v > best_v) { best_k = k; best_v = v; }
        }
        return best_k;
    };

    if (!msf_long_counts.empty())
        audio_ref.dominant_msf_long = max_key(msf_long_counts);
    if (!msf_short_counts.empty())
        audio_ref.dominant_msf_short = max_key(msf_short_counts);
    if (!ms_counts.empty())
        audio_ref.dominant_ms = max_key(ms_counts);
}

} // anonymous namespace


ScanResult scan_mdat(const std::string& corrupted_path, ReferenceInfo& ref) {
    MappedFile mf(corrupted_path);
    const uint8_t* file_data = mf.data();
    const uint64_t file_size = mf.size();
    const uint32_t spc = ref.samples_per_chunk;

    ScanResult result;

    // Locate mdat atom
    uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        uint32_t box_size = read_be32(file_data + pos);
        bool is_mdat = std::memcmp(file_data + pos + 4, "mdat", 4) == 0;

        uint64_t actual_size = box_size;
        if (box_size == 1 && pos + 16 <= file_size) {
            actual_size = read_be64(file_data + pos + 8);
        } else if (box_size == 0) {
            actual_size = file_size - pos;
        }

        if (is_mdat) {
            result.mdat_offset = pos;
            result.mdat_size = actual_size;
            break;
        }

        if (actual_size < 8) break;
        pos += actual_size;
    }

    if (result.mdat_size == 0)
        throw std::runtime_error("No mdat atom found");

    const uint64_t mdat_end = result.mdat_offset + result.mdat_size;
    const uint64_t data_start = result.mdat_offset + 16; // skip mdat header + 8-byte preamble

    std::println("  mdat: offset={}, size={:.1f} MB",
                 result.mdat_offset, result.mdat_size / (1024.0 * 1024.0));

    // Pointers into the mmap'd data
    const uint8_t* const mdat_end_ptr = file_data + mdat_end;

    // Access unit state
    uint64_t au_start = 0;
    uint32_t au_size = 0;
    bool au_is_sync = false;
    bool au_has_slice = false;
    const uint8_t* au_slice_hdr = nullptr;

    auto finalize_au = [&]() {
        if (au_size > 0 && au_has_slice) {
            uint8_t is_b = 0;
            if (au_slice_hdr) {
                int st = parse_slice_type(au_slice_hdr);
                if (st == 1 || st == 6) {
                    is_b = 1;
                    result.has_b_frames = true;
                }
            }
            result.video_samples.push_back({au_start, au_size});
            result.slice_types.push_back(is_b);
            // chunk_sample_indices handled at chunk level
            if (au_is_sync) {
                result.sync_samples.push_back(
                    static_cast<uint32_t>(result.video_samples.size()));
            }
        }
    };

    // Track per-chunk sample indices
    std::vector<uint32_t> chunk_sample_indices;

    // Track whether bootstrap is needed (no reference stats)
    bool needs_bootstrap = !ref.audio_ref.dominant_msf_long.has_value() &&
                           ref.audio_ref.frame_size_mean <= 0;

    const uint8_t* p = file_data + data_start;
    std::println("  Scanning mdat for video/audio samples...");

    while (p + 5 < mdat_end_ptr) {
        // ── Parse one video chunk ──
        uint64_t chunk_offset = static_cast<uint64_t>(p - file_data);
        chunk_sample_indices.clear();
        au_start = chunk_offset;
        au_size = 0;
        au_is_sync = false;
        au_has_slice = false;
        au_slice_hdr = nullptr;

        while (p + 4 < mdat_end_ptr) {
            if (p + 5 > mdat_end_ptr) break;

            uint32_t nal_len = read_be32(p);
            uint8_t nal_byte = p[4];
            uint8_t nal_type = nal_byte & 0x1F;

            uint64_t abs_pos = static_cast<uint64_t>(p - file_data);
            if (nal_len < 1 || nal_len > MAX_NAL_SIZE ||
                !((VALID_NAL_MASK >> nal_type) & 1) ||
                (nal_byte & 0x80) ||
                abs_pos + 4 + nal_len > mdat_end)
                break;

            uint32_t nal_total = 4 + nal_len;

            if (nal_type == 9) { // AUD → new access unit
                // Save current chunk sample index before finalize
                size_t pre_count = result.video_samples.size();
                finalize_au();
                if (result.video_samples.size() > pre_count) {
                    chunk_sample_indices.push_back(
                        static_cast<uint32_t>(result.video_samples.size() - 1));
                }
                au_start = abs_pos;
                au_size = nal_total;
                au_is_sync = false;
                au_has_slice = false;
                au_slice_hdr = nullptr;
            } else {
                au_size += nal_total;
                if (nal_type == 5) { // IDR slice
                    au_is_sync = true;
                    au_has_slice = true;
                    if (!au_slice_hdr && p + 9 <= mdat_end_ptr) {
                        au_slice_hdr = p + 5;
                    }
                } else if (nal_type <= 4) { // Coded slice (types 1-4)
                    au_has_slice = true;
                    if (!au_slice_hdr && p + 9 <= mdat_end_ptr) {
                        au_slice_hdr = p + 5;
                    }
                }
            }

            p += nal_total;
        }

        // Finalize last AU in chunk
        size_t pre_count = result.video_samples.size();
        finalize_au();
        if (result.video_samples.size() > pre_count) {
            chunk_sample_indices.push_back(
                static_cast<uint32_t>(result.video_samples.size() - 1));
        }

        if (!chunk_sample_indices.empty()) {
            result.video_chunks.push_back({chunk_offset, chunk_sample_indices});
        }

        // ── Find next video chunk (skip audio region) ──
        uint64_t audio_start = static_cast<uint64_t>(p - file_data);
        const uint8_t* next_video = find_next_video(p, mdat_end_ptr, file_data, mdat_end);

        if (!next_video) {
            if (p < mdat_end_ptr) {
                create_audio_chunk(file_data, audio_start, mdat_end, spc,
                                   result.audio_samples, result.audio_chunks,
                                   ref.audio_ref);
            }
            break;
        }

        uint64_t next_video_offset = static_cast<uint64_t>(next_video - file_data);
        if (next_video_offset > audio_start) {
            create_audio_chunk(file_data, audio_start, next_video_offset, spc,
                               result.audio_samples, result.audio_chunks,
                               ref.audio_ref);
        }

        // Bootstrap audio stats from first chunks (reference-free mode)
        if (needs_bootstrap && result.audio_chunks.size() >= 10) {
            bootstrap_audio_ref(file_data, result.audio_samples, ref.audio_ref);
            // Re-process first chunks with proper stats
            std::vector<std::pair<uint64_t, uint64_t>> chunk_regions;
            for (const auto& ch : result.audio_chunks) {
                auto& s0 = result.audio_samples[ch.sample_indices.front()];
                auto& sN = result.audio_samples[ch.sample_indices.back()];
                chunk_regions.emplace_back(s0.offset, sN.offset + sN.size);
            }
            result.audio_samples.clear();
            result.audio_chunks.clear();
            for (auto [cstart, cend] : chunk_regions) {
                create_audio_chunk(file_data, cstart, cend, spc,
                                   result.audio_samples, result.audio_chunks,
                                   ref.audio_ref);
            }
            needs_bootstrap = false;
        }

        p = next_video;

        if (result.video_chunks.size() % 500 == 0) {
            double pct = static_cast<double>(p - file_data - data_start)
                       / (mdat_end - data_start) * 100.0;
            std::println("    {:L} video, {:L} audio samples ({:.1f}%)",
                         result.video_samples.size(), result.audio_samples.size(), pct);
        }
    }

    std::println("  Found {:L} video samples in {:L} chunks",
                 result.video_samples.size(), result.video_chunks.size());
    std::println("  Found {:L} audio samples in {:L} chunks",
                 result.audio_samples.size(), result.audio_chunks.size());
    std::println("  Keyframes: {:L}", result.sync_samples.size());

    int b_count = 0;
    for (auto t : result.slice_types) b_count += t;
    std::println("  B-frames detected: {} / {}", b_count, result.slice_types.size());

    return result;
}

} // namespace recover
