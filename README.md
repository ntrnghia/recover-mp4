# recover-mp4

Recovers corrupted MP4 files missing the **moov atom** by scanning raw mdat data for H.264 NAL units and AAC audio, then rebuilding the MP4 index from scratch.

Built for **Windows Snipping Tool** recordings but works with any MP4 using H.264 + AAC-LC interleaved as `[V chunk][A chunk][V chunk][A chunk]...`.

## Requirements

- **Python 3.10+** (no external packages — stdlib only)
- **FFmpeg** in PATH (for audio fix step)
- A **working reference MP4** from the same app/settings

## Usage

```powershell
python -m recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]
```

If `output.mp4` is omitted, writes to `<corrupted>_recovered.mp4`.

## How It Works

1. **Parse reference** — extracts codec config (SPS/PPS), timescales, timing, audio frame size statistics (min/max/mean/stdev), and dominant AAC header patterns (max_sfb, ms_mask_present) from a working MP4
2. **Scan mdat** — single O(n) pass over corrupted file:
   - Parses length-prefixed H.264 NAL units sequentially
   - Groups NALs into access units (frames) using AUD delimiters
   - Detects IDR keyframes and B-frames via exp-Golomb slice header parsing
   - When invalid data is hit → end of video chunk → searches for next AUD pattern
   - Everything between video chunks = audio → detects AAC frame boundaries via quality-scored DP partitioning using reference-derived max_sfb and ms_mask_present patterns
3. **Build moov** — constructs complete MP4 index (mvhd, trak, stbl tables) from scan results
4. **Write output** — streams pre-mdat boxes + mdat + new moov
5. **Fix audio** — hybrid segment-based approach using ffmpeg:
   - Extracts full audio track once (stream copy), then works from the small audio file
   - Splits audio into 2-second segments and tests in parallel (8 threads)
   - Tests each segment for AAC decode errors
   - Clean segments → lossless copy (preserves original quality)
   - Corrupt segments → re-encodes with error tolerance (AAC 192k stereo)
   - Crash-prone segments → replaced with silence
   - Concatenates all segments and muxes with original video

## Package Structure

```
recover_mp4/
├── __init__.py      # Package marker
├── __main__.py      # CLI entry point
├── constants.py     # AUD_PATTERN, VALID_NAL_MASK, parse_slice_type
├── reference.py     # Reference MP4 parser (SPS/PPS, timescales, AAC patterns)
├── scanner.py       # mdat scanner (video/audio sample detection)
├── atoms.py         # MP4 atom builders (moov, trak, stbl boxes)
├── writer.py        # Output writer + ffmpeg hybrid audio fixer
└── README.md        # This file
```

## Key Optimizations

| Optimization | Location | Impact |
|---|---|---|
| 4 MB buffered reads + `struct.unpack_from` | `scanner.py` | ~500K syscalls → ~500 buffer refills |
| Pre-compiled `struct.Struct('>I')` | `scanner.py` | Eliminates format parsing per NAL header |
| Bitmask NAL validation (`0x1FFE >> type & 1`) | `constants.py` | Single bitshift vs hash lookup |
| C-level `bytes.find()` for AUD/AAC search | `scanner.py` | Replaces Python-level byte iteration |
| `array.array` bulk packing + `byteswap()` | `atoms.py` | O(1) endian conversion for stsz/stco/stss |
| Inlined exp-Golomb (no closure) | `constants.py` | No function object creation per frame |
| DP candidate cap at 500 + bisect bounds | `scanner.py` | Keeps AAC boundary detection tractable |
| Quality-scored DP with reference patterns | `scanner.py` | max_sfb/ms_mask matching eliminates false boundaries |
| Precomputed DP cost terms (`inv_scale`, `scaled_scores`) | `scanner.py` | Eliminates division and multiplication per DP iteration |
| Batch chunk reads for AAC pattern extraction | `reference.py` | Single read per chunk vs per-frame seek |
| Extract audio once + parallel segment processing | `writer.py` | Avoids 1000+ seeks into large file; 8-thread parallel test/fix |

## AAC Boundary Detection

Each audio region between video chunks contains ~15 raw AAC-LC frames with no length prefixes. Boundaries are detected via dynamic programming:

1. **Candidate search** — scans for bytes `0x20`/`0x21` (CPE element with common_window=0/1) and validates AAC-LC header fields (ics_reserved_bit, window_sequence, max_sfb, predictor_data_present, ms_mask_present)
2. **Quality scoring** — each candidate gets a score based on how well it matches the reference file's dominant header patterns:
   - common_window=1: +3 (dominant for stereo)
   - ONLY_LONG window: +2 (most common window type)
   - Matching max_sfb: +6 (strongest discriminator — nearly perfect)
   - Matching ms_mask_present: +3
   - Wrong max_sfb: −3, EIGHT_SHORT window: −1, reserved ms_mask=3: rejected
3. **DP partition** — finds the partition into `n` frames minimizing `Σ(z² − quality_bonus)` where `z = (frame_size − ref_mean) / ref_stdev`, with frame sizes constrained to reference-derived bounds
4. **Multi-pass** — tries tight bounds (ref_min×0.7 to ref_max×1.3) first, falls back to loose bounds

## Algorithm Complexity

| Stage | Time | Space |
|---|---|---|
| Reference parse | O(moov_size + audio_data) | O(moov_size) |
| mdat scan | O(file_size) | O(samples) |
| AAC boundaries (per chunk) | O(n × c × log c) | O(n × c) |
| moov build | O(samples) | O(samples) |
| File write | O(file_size) | O(1) streaming |

Where n = frames per chunk (~15), c = candidates (capped at 500).

## Reference File Requirements

- Same **app** (e.g., Windows Snipping Tool)
- Same **resolution** and **frame rate**
- Same **codec** (H.264 + AAC)
- **Not corrupted** (plays normally)
- Shorter files (1-10 min) tend to have cleaner metadata
