# recover-mp4

Recovers corrupted MP4 files missing the **moov atom** by scanning raw mdat data for H.264 video and AAC audio samples, then rebuilding the MP4 index from scratch.

Built for **Windows Snipping Tool** recordings (H.264 + AAC-LC, interleaved `[V][A][V][A]...`).

No reference file needed — codec config is auto-detected from an encoder SEI embedded in the corrupted file's mdat.

Available in **Python** and **C++**. Both produce identical output.

## Requirements

### Python

- Python 3.10+ (stdlib only)
- FFmpeg in PATH (audio fix)

### C++

- C++26 (GCC 15+)
- CMake 3.25+
- FFmpeg dev libs (`libavformat`, `libavcodec`, `libavutil`) + CLI
- pkg-config

## Usage

### Python

```
python -m recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]
```

### C++

```bash
cd cpp && mkdir build && cd build && cmake .. && make -j$(nproc)
./recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]
```

Reference is optional. If omitted, codec config is auto-detected from the corrupted file. If `output.mp4` is omitted, writes to `<corrupted>_recovered.mp4`.

## How It Works

1. **Detect config** — reads encoder SEI (type 5, user_data_unregistered) from mdat to get resolution, entropy mode, and reference frame count. Constructs SPS/PPS from these parameters. Falls back to a reference MP4 if provided.
2. **Scan mdat** — single O(n) pass:
   - Parses length-prefixed H.264 NALs, groups into access units via AUD delimiters
   - Detects IDR keyframes and B-frames via exp-Golomb slice header parsing
   - Invalid NAL → end of video chunk → searches for next AUD pattern
   - Gap between video chunks = audio → splits into AAC frames via quality-scored DP partitioning
3. **Bootstrap audio stats** — after 10 chunks, analyzes equal-split frames to extract dominant AAC header patterns (max_sfb, ms_mask) and frame size statistics, then re-processes those chunks with proper boundary detection
4. **Build moov** — constructs MP4 index (mvhd, trak, stbl) from scan results. Uses 64-bit boxes for recordings >24 min.
5. **Write output** — streams pre-mdat boxes + mdat + moov
6. **Fix audio** — parallel hybrid approach:
   - Extracts audio track, splits into 2s segments, tests each for AAC errors (8 threads)
   - Clean → lossless copy. Corrupt → re-encode (AAC 192k). Failed → silence.
   - Concatenates and muxes with original video

## AAC Boundary Detection

Each audio chunk contains ~15 raw AAC-LC frames with no length prefixes. Boundaries are found via DP:

1. **Candidates** — scans for CPE headers (`0x20`/`0x21`), validates AAC-LC fields (ics_reserved_bit, window_sequence, max_sfb, predictor, ms_mask)
2. **Scoring** — candidates matching the bootstrapped dominant patterns get higher scores (max_sfb match: +6, ms_mask match: +3, wrong max_sfb: −3)
3. **DP partition** — minimizes Σ((frame_size − μ) / σ)² − quality_bonus, with frame size bounds from reference stats
4. **Fallback** — tight bounds first, then loose. Equal splitting if DP fails.

## Encoder Details

Microsoft H.264 Encoder V1.5.3 (MFT) + Microsoft AAC Audio Encoder.

Two SPS/PPS variants exist, distinguished by `cabac` field in the SEI:

| | Variant A (CAVLC, common) | Variant B (CABAC, rare) |
|---|---|---|
| Entropy coding | CAVLC | CABAC |
| pic_order_cnt_type | 0 | 2 |
| max_num_ref_frames | 2 | 1 |
| VUI num_units_in_tick | 1 | 1000 |
| Encoder SEI present | Yes | No |
| PPS bytes | `68 CE 3C 80` | `68 EE 3C 80` |

Fixed parameters across all recordings:
- Profile: Main (77), Level: 4.0 (≤1080p) or 5.0 (1440p+)
- 30 fps, timescale 30000, sample delta 1000
- Audio: AAC-LC, 48kHz stereo, ~192kbps CBR, 1024 samples/frame, 15 frames/chunk
- Interleaving: strict V-A-V-A alternation
- No B-frames, no inline SPS/PPS in mdat

## Project Structure

```
recover_mp4/
├── __main__.py      # CLI entry point
├── constants.py     # NAL patterns, slice type parser
├── reference.py     # Reference parser + SEI auto-detection
├── scanner.py       # mdat scanner (video/audio detection + bootstrap)
├── atoms.py         # moov atom builder (64-bit box support)
├── writer.py        # Output writer + parallel audio fixer
└── cpp/             # C++ port (identical algorithm, mmap I/O)
    ├── CMakeLists.txt
    └── src/{main,reference,scanner,atoms,writer,constants,mmap_file}.*
```
