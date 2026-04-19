/// MP4 Recovery Tool for Windows Snipping Tool recordings.
///
/// Recovers corrupted MP4 files missing the moov atom by scanning the mdat
/// for H.264 NAL units and AAC audio, then rebuilding the index.
///
/// Usage:
///     recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]

#include "reference.hpp"
#include "scanner.hpp"
#include "atoms.hpp"
#include "writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::println("Usage: recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]");
        return 1;
    }

    std::string corrupted = argv[1];
    std::string reference = argv[2];
    std::string output;
    if (argc > 3) {
        output = argv[3];
    } else {
        auto p = std::filesystem::path(corrupted);
        output = (p.parent_path() / (p.stem().string() + "_recovered.mp4")).string();
    }

    if (!std::filesystem::exists(corrupted)) {
        std::println("Error: {} not found", corrupted);
        return 1;
    }
    if (!std::filesystem::exists(reference)) {
        std::println("Error: {} not found", reference);
        return 1;
    }

    try {
        std::println("[1/5] Parsing reference file...");
        auto ref = recover::parse_reference(reference);
        std::println("  Video: {}x{}", ref.width, ref.height);
        std::println("  SPS: {} bytes, PPS: {} bytes", ref.sps.size(), ref.pps.size());
        double fps = static_cast<double>(ref.video_timescale) / ref.video_sample_delta;
        std::println("  Video: timescale={}, delta={} ({:.1f} fps)",
                     ref.video_timescale, ref.video_sample_delta, fps);
        std::println("  Audio: timescale={}, delta={}, samples_per_chunk={}",
                     ref.audio_timescale, ref.audio_sample_delta, ref.samples_per_chunk);
        std::println("  mvhd timescale: {}", ref.mvhd_timescale);

        std::println("\n[2/5] Scanning corrupted file...");
        auto scan = recover::scan_mdat(corrupted, ref);

        std::println("\n[3/5] Building moov atom...");
        auto moov = recover::build_moov(ref, scan);
        std::println("  moov size: {:L} bytes", moov.size());

        std::println("\n[4/5] Writing output file...");
        recover::write_output(corrupted, output, scan, moov);

        std::println("\n[5/5] Fixing audio (FFmpeg re-encode)...");
        bool ok = recover::fix_audio(output);
        if (ok) {
            std::println("  Audio fixed successfully.");
        }

        auto vs = scan.video_samples.size();
        auto aus = scan.audio_samples.size();
        auto kf = scan.sync_samples.size();
        double dur_s = static_cast<double>(vs) * ref.video_sample_delta / ref.video_timescale;
        int dur_m = static_cast<int>(dur_s) / 60;
        int dur_sec = static_cast<int>(dur_s) % 60;
        double size_mb = static_cast<double>(std::filesystem::file_size(output))
                        / (1024.0 * 1024.0);

        std::println("\n{:=<60}", "");
        std::println("Recovery complete: {}", output);
        std::println("  Output size:   {:.1f} MB", size_mb);
        std::println("  Video samples: {:L}", vs);
        std::println("  Audio samples: {:L}", aus);
        std::println("  Keyframes:     {:L}", kf);
        std::println("  Duration:      {}m {}s", dur_m, dur_sec);

    } catch (const std::exception& e) {
        std::println("ERROR: {}", e.what());
        return 1;
    }

    return 0;
}
