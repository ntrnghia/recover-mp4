/// Output file writer and ffmpeg subprocess audio fixer.
///
/// write_output: streams mdat from mmap'd source to output file, appends moov.
/// fix_audio: hybrid parallel audio fix — extract once, test/fix segments in parallel.

#include "writer.hpp"
#include "constants.hpp"
#include "mmap_file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <print>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace recover {

void write_output(const std::string& corrupted_path, const std::string& output_path,
                  const ScanResult& scan, const std::vector<uint8_t>& moov) {

    MappedFile src(corrupted_path);
    const uint8_t* data = src.data();

    std::ofstream dst(output_path, std::ios::binary);
    if (!dst)
        throw std::runtime_error("Cannot open output file: " + output_path);

    // Copy pre-mdat boxes (ftyp, uuid, etc.)
    dst.write(reinterpret_cast<const char*>(data), scan.mdat_offset);

    // Detect original mdat header size
    uint32_t orig_size_field = read_be32(data + scan.mdat_offset);
    size_t orig_hdr_size = (orig_size_field == 1) ? 16 : 8;

    // Write mdat header.
    // data_bytes = actual payload bytes (mdat_size includes the original header).
    uint64_t data_bytes = scan.mdat_size - orig_hdr_size;
    if (scan.mdat_size > 0xFFFFFFFF) {
        // Extended 64-bit size: total box = 16-byte header + data_bytes.
        uint8_t hdr[16];
        write_be32(hdr, 1);
        std::memcpy(hdr + 4, "mdat", 4);
        write_be64(hdr + 8, 16 + data_bytes);
        dst.write(reinterpret_cast<const char*>(hdr), 16);
    } else {
        uint8_t hdr[8];
        write_be32(hdr, static_cast<uint32_t>(orig_hdr_size + data_bytes));
        std::memcpy(hdr + 4, "mdat", 4);
        dst.write(reinterpret_cast<const char*>(hdr), 8);
    }

    // Copy mdat content (skip original header)
    const uint8_t* mdat_content = data + scan.mdat_offset + orig_hdr_size;
    dst.write(reinterpret_cast<const char*>(mdat_content), static_cast<std::streamsize>(data_bytes));

    // Append moov
    dst.write(reinterpret_cast<const char*>(moov.data()), moov.size());
}


// ─── Cross-platform subprocess helpers ──────────────────────────────────────

namespace {

/// Run ffmpeg command and capture stderr. Returns exit code.
int run_ffmpeg(const std::vector<std::string>& args, std::string* captured_stderr = nullptr) {
    std::string cmd;
    for (const auto& a : args) {
        if (!cmd.empty()) cmd += ' ';
        // Quote arguments containing spaces or special chars
        if (a.find(' ') != std::string::npos || a.find('\'') != std::string::npos) {
            cmd += '"';
            cmd += a;
            cmd += '"';
        } else {
            cmd += a;
        }
    }

    // Redirect stderr to a temp file if we need to capture it
    std::string stderr_file;
    if (captured_stderr) {
        static std::atomic<uint64_t> stderr_counter{0};
        auto tmp = std::filesystem::temp_directory_path() /
                   std::format("ffmpeg_err_{}_{}.txt",
                               std::this_thread::get_id(),
                               stderr_counter.fetch_add(1, std::memory_order_relaxed));
        stderr_file = tmp.string();
        cmd += " 2>\"" + stderr_file + "\"";
    } else {
#ifdef _WIN32
        cmd += " 2>NUL";
#else
        cmd += " 2>/dev/null";
#endif
    }
#ifdef _WIN32
    cmd += " >NUL";
#else
    cmd += " >/dev/null";
#endif

    int ret = std::system(cmd.c_str());

    if (captured_stderr && !stderr_file.empty()) {
        std::ifstream ifs(stderr_file);
        if (ifs) {
            *captured_stderr = std::string(
                std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>());
        }
        std::error_code ec;
        std::filesystem::remove(stderr_file, ec);
    }

    return ret;
}

/// Probe file duration in seconds using ffmpeg.
double probe_duration(const std::string& path) {
    std::string output;
    run_ffmpeg({"ffmpeg", "-i", path}, &output);

    std::regex dur_re(R"(Duration:\s*(\d+):(\d+):(\d+)\.(\d+))");
    std::smatch m;
    if (std::regex_search(output, m, dur_re)) {
        return std::stoi(m[1]) * 3600.0 + std::stoi(m[2]) * 60.0 +
               std::stoi(m[3]) + std::stoi(m[4]) / 100.0;
    }
    return 0.0;
}

/// Count AAC errors in ffmpeg stderr output.
int count_aac_errors(const std::string& stderr_output) {
    int count = 0;
    std::istringstream iss(stderr_output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("aac") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

/// Remove file if it exists (no-throw).
void try_remove(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

/// Print progress bar for segment processing.
void print_progress(int done, int total, int lossless, int reencoded, int silent,
                    double elapsed, const std::string& action) {
    double pct = (total > 0) ? 100.0 * done / total : 0;
    int filled = (total > 0) ? 30 * done / total : 0;
    double eta = (done > 0) ? elapsed / done * (total - done) : 0;
    std::string bar(filled, '#');
    bar.append(30 - filled, '-');
    std::print("\r  [{}] {}/{} ({:.0f}%) L:{} R:{} S:{} [{}] "
               "elapsed:{:.0f}s eta:{:.0f}s   ",
               bar, done, total, pct, lossless, reencoded, silent,
               action, elapsed, eta);
    std::fflush(stdout);
}

} // anonymous namespace


bool fix_audio(const std::string& output_path) {
    // Check if ffmpeg is available
#ifdef _WIN32
    if (std::system("ffmpeg -version >NUL 2>NUL") != 0) {
#else
    if (std::system("ffmpeg -version >/dev/null 2>/dev/null") != 0) {
#endif
        std::println("  WARNING: ffmpeg not found — skipping audio fix.");
        std::println("  Run manually: ffmpeg -i OUTPUT -c:v copy -c:a aac -b:a 192k FIXED.mp4");
        return false;
    }

    // Check if audio decodes cleanly
    std::print("  Validating audio stream...");
    std::fflush(stdout);
    auto t_chk = std::chrono::steady_clock::now();
    std::string stderr_out;
    run_ffmpeg({"ffmpeg", "-v", "error", "-i", output_path, "-vn", "-f", "null", "-"},
               &stderr_out);
    int aac_errors = count_aac_errors(stderr_out);
    double chk_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_chk).count();
    if (aac_errors == 0) {
        std::println(" clean. ({:.1f}s)", chk_elapsed);
        return true;
    }
    std::println(" {} AAC errors. ({:.1f}s)", aac_errors, chk_elapsed);
    std::println("  Re-encoding audio...");

    // ── Hybrid parallel audio fix ──
    auto t0 = std::chrono::steady_clock::now();

    double duration = probe_duration(output_path);
    if (duration <= 0) {
        std::println("  WARNING: Could not determine duration.");
        return false;
    }

    constexpr double seg_dur = 2.0;
    int total_segs = static_cast<int>(duration / seg_dur) +
                     ((std::fmod(duration, seg_dur) > 0) ? 1 : 0);
    int workers = std::min(8, static_cast<int>(std::thread::hardware_concurrency()));
    if (workers < 1) workers = 4;

    auto tmpdir = std::filesystem::temp_directory_path() / "recover_mp4_cpp";
    std::error_code ec;
    std::filesystem::create_directories(tmpdir, ec);

    auto cleanup = [&]() {
        std::error_code ec2;
        std::filesystem::remove_all(tmpdir, ec2);
    };

    // Phase 1: Extract full audio track
    std::print("  Extracting audio track...");
    std::fflush(stdout);
    std::string full_audio = (tmpdir / "full_audio.m4a").string();
    int ret = run_ffmpeg({"ffmpeg", "-y", "-v", "error", "-i", output_path,
                          "-vn", "-c:a", "copy", full_audio});
    if (ret != 0) {
        std::println(" failed.");
        cleanup();
        return false;
    }
    std::println(" done.");

    // Build segment file paths
    std::vector<std::string> seg_files(total_segs);
    for (int i = 0; i < total_segs; ++i) {
        seg_files[i] = (tmpdir / std::format("seg_{:05d}.m4a", i)).string();
    }

    // Phase 2: Extract + test segments in parallel
    auto extract_and_test = [&](int i) -> bool {
        double ss = i * seg_dur;
        double t = std::min(seg_dur, duration - ss);
        const auto& seg_path = seg_files[i];

        int r = run_ffmpeg({"ffmpeg", "-y", "-v", "error",
                            "-ss", std::format("{:.3f}", ss),
                            "-t", std::format("{:.3f}", t),
                            "-i", full_audio, "-vn", "-c:a", "copy", seg_path});
        if (r != 0 || !std::filesystem::exists(seg_path)) return false;

        std::string err;
        run_ffmpeg({"ffmpeg", "-v", "error", "-i", seg_path, "-f", "null", "-"}, &err);
        return count_aac_errors(err) == 0;
    };

    std::vector<bool> seg_good(total_segs, false);
    {
        std::vector<std::future<bool>> futures;
        int done = 0;
        for (int start = 0; start < total_segs; start += workers) {
            int batch_end = std::min(start + workers, total_segs);
            futures.clear();
            for (int i = start; i < batch_end; ++i) {
                futures.push_back(std::async(std::launch::async, extract_and_test, i));
            }
            for (int i = 0; i < static_cast<int>(futures.size()); ++i) {
                seg_good[start + i] = futures[i].get();
                ++done;
                if (done % 20 == 0 || done == total_segs) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - t0).count();
                    int good_count = 0;
                    for (int j = 0; j < done; ++j) if (seg_good[j]) ++good_count;
                    print_progress(done, total_segs, good_count, 0, 0, elapsed, "testing");
                }
            }
        }
    }

    // Phase 3: Re-encode bad segments in parallel
    std::vector<int> bad_indices;
    for (int i = 0; i < total_segs; ++i) {
        if (!seg_good[i]) bad_indices.push_back(i);
    }

    std::atomic<int> reencoded{0}, silent_count{0};

    auto fix_seg = [&](int i) {
        double ss = i * seg_dur;
        double t = std::min(seg_dur, duration - ss);
        const auto& seg_path = seg_files[i];
        std::string tmp_path = seg_path + ".tmp.m4a";

        int r = run_ffmpeg({"ffmpeg", "-y", "-v", "error",
                            "-err_detect", "ignore_err",
                            "-fflags", "+genpts+discardcorrupt",
                            "-ss", std::format("{:.3f}", ss),
                            "-t", std::format("{:.3f}", t),
                            "-i", full_audio,
                            "-c:a", "aac", "-ac", "2", "-b:a", "192k", tmp_path});
        if (r == 0 && std::filesystem::exists(tmp_path)) {
            std::filesystem::rename(tmp_path, seg_path);
            ++reencoded;
            return;
        }
        try_remove(tmp_path);
        // Silence fallback
        run_ffmpeg({"ffmpeg", "-y", "-v", "error",
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo",
                    "-t", std::format("{:.3f}", t),
                    "-c:a", "aac", "-b:a", "192k", seg_path});
        ++silent_count;
    };

    if (!bad_indices.empty()) {
        std::print("\n  Re-encoding {} bad segments...", bad_indices.size());
        std::fflush(stdout);
        auto t_fix = std::chrono::steady_clock::now();
        std::vector<std::future<void>> futures;
        for (int start = 0; start < static_cast<int>(bad_indices.size()); start += workers) {
            int batch_end = std::min(start + workers, static_cast<int>(bad_indices.size()));
            futures.clear();
            for (int i = start; i < batch_end; ++i) {
                futures.push_back(std::async(std::launch::async, fix_seg, bad_indices[i]));
            }
            for (auto& f : futures) f.get();
        }
        double fix_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_fix).count();
        std::println(" done. ({:.1f}s)", fix_elapsed);
    }

    int lossless = total_segs - static_cast<int>(bad_indices.size());
    std::println("\r  Segments: {}/{} lossless, {}/{} re-encoded, "
                 "{}/{} silence.{:>20}",
                 lossless, total_segs, reencoded.load(), total_segs,
                 silent_count.load(), total_segs, "");

    // Phase 4: Concatenate all segments
    std::print("  Concatenating segments...");
    std::fflush(stdout);
    std::string concat_list = (tmpdir / "concat.txt").string();
    {
        std::ofstream cl(concat_list);
        for (const auto& seg : seg_files) {
            cl << "file '" << seg << "'\n";
        }
    }
    std::string concat_audio = (tmpdir / "concat_audio.m4a").string();
    ret = run_ffmpeg({"ffmpeg", "-y", "-v", "error",
                      "-f", "concat", "-safe", "0", "-i", concat_list,
                      "-c:a", "copy", concat_audio});
    if (ret != 0) {
        std::println(" failed.");
        cleanup();
        return false;
    }
    std::println(" done.");

    // Phase 5: Mux original video + fixed audio
    std::print("  Muxing video + fixed audio...");
    std::fflush(stdout);
    std::string tmp = output_path + ".tmp.mp4";
    ret = run_ffmpeg({"ffmpeg", "-y", "-v", "error",
                      "-i", output_path, "-i", concat_audio,
                      "-map", "0:v", "-map", "1:a",
                      "-c:v", "copy", "-c:a", "copy",
                      "-movflags", "+faststart", tmp});
    if (ret == 0 && std::filesystem::exists(tmp)) {
        std::filesystem::rename(tmp, output_path);
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        std::println(" done. ({:.1f}s)", elapsed);
        std::println("  Audio fixed.");
        cleanup();
        return true;
    }

    std::println(" failed.");
    try_remove(tmp);
    cleanup();
    return false;
}

} // namespace recover
