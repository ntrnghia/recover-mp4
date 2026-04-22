/// MP4 Recovery Tool for Windows Snipping Tool recordings.
///
/// Recovers corrupted MP4 files missing the moov atom by scanning the mdat
/// for H.264 NAL units and AAC audio, then rebuilding the index.
///
/// Usage:
///     recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]

#include "reference.hpp"
#include "scanner.hpp"
#include "atoms.hpp"
#include "writer.hpp"
#include "constants.hpp"
#include "mmap_file.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("Usage: recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]");
        return 1;
    }

    std::string corrupted = argv[1];
    std::string reference;
    std::string output;

    // Determine if second arg is reference or output
    if (argc >= 3) {
        std::string arg2 = argv[2];
        if (argc >= 4) {
            reference = arg2;
            output = argv[3];
        } else if (std::filesystem::exists(arg2) && arg2.ends_with(".mp4")) {
            reference = arg2;
        } else {
            output = arg2;
        }
    }

    if (output.empty()) {
        auto p = std::filesystem::path(corrupted);
        output = (p.parent_path() / (p.stem().string() + "_recovered.mp4")).string();
    }

    if (!std::filesystem::exists(corrupted)) {
        std::println("Error: {} not found", corrupted);
        return 1;
    }

    try {
        recover::ReferenceInfo ref;
        if (!reference.empty()) {
            std::println("[1/5] Parsing reference file...");
            ref = recover::parse_reference(reference);
        } else {
            std::println("[1/5] Auto-detecting codec config from corrupted file...");
            ref = recover::detect_config(corrupted);
        }
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

        // Adjust chunk offsets if mdat header size changes in output.
        // Original file may have 8-byte header (size=0), but >4GB mdat needs
        // a 16-byte extended header in output — shifting all data offsets by +8.
        {
            recover::MappedFile probe(corrupted);
            uint32_t orig_sf = recover::read_be32(probe.data() + scan.mdat_offset);
            size_t orig_hdr = (orig_sf == 1) ? 16 : 8;
            size_t new_hdr  = (scan.mdat_size > 0xFFFFFFFFull) ? 16 : 8;
            int64_t delta   = static_cast<int64_t>(new_hdr) - static_cast<int64_t>(orig_hdr);
            if (delta != 0) {
                std::println("  mdat header: {}B\u2192{}B, adjusting chunk offsets by +{}",
                             orig_hdr, new_hdr, delta);
                for (auto& c : scan.video_chunks) c.offset += delta;
                for (auto& c : scan.audio_chunks) c.offset += delta;
            }
        }

        std::println("\n[3/5] Building moov atom...");
        auto moov = recover::build_moov(ref, scan);
        std::println("  moov size: {:L} bytes", moov.size());

        std::print("\n[4/5] Writing output file...");
        std::fflush(stdout);
        auto t_write = std::chrono::steady_clock::now();
        recover::write_output(corrupted, output, scan, moov);
        double w_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_write).count();
        auto file_size = std::filesystem::file_size(output);
        std::println(" {} MB written. ({:.1f}s)", file_size / (1024 * 1024), w_elapsed);

        std::println("\n[5/5] Fixing audio...");
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
