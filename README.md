# recover_mp4

Recovers corrupted MP4 files missing the **moov atom** by scanning raw mdat data for H.264 NAL units and AAC audio, then rebuilding the MP4 index from scratch.

Built for **Windows Screen Sketch** recordings but works with any MP4 using H.264 + AAC-LC interleaved as `[V chunk][A chunk][V chunk][A chunk]...`.

## Requirements

- **Python 3.10+** (no external packages — stdlib only)
- **FFmpeg** in PATH (for audio re-encode fix)
- A **working reference MP4** from the same app/settings

## Usage

```powershell
# As module (recommended)
python -m recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]

# Via wrapper script
python recover_mp4_v3.py <corrupted.mp4> <reference.mp4> [output.mp4]
```

If `output.mp4` is omitted, writes to `<corrupted>_recovered.mp4`.

## How It Works

1. **Parse reference** — extracts codec config (SPS/PPS), timescales, and timing from a working MP4's moov atom
2. **Scan mdat** — single O(n) pass over corrupted file:
   - Parses length-prefixed H.264 NAL units sequentially
   - Groups NALs into access units (frames) using AUD delimiters
   - Detects IDR keyframes and B-frames via exp-Golomb slice header parsing
   - When invalid data is hit → end of video chunk → searches for next AUD pattern
   - Everything between video chunks = audio → detects AAC frame boundaries via DP
3. **Build moov** — constructs complete MP4 index (mvhd, trak, stbl tables) from scan results
4. **Write output** — streams pre-mdat boxes + mdat + new moov
5. **Fix audio** — ffmpeg re-encodes audio (video untouched) to fix imprecise AAC frame boundaries

## Package Structure

```
recover_mp4/
├── __init__.py      # Package marker
├── __main__.py      # CLI entry point
├── constants.py     # AUD_PATTERN, VALID_NAL_MASK, parse_slice_type
├── reference.py     # Reference MP4 parser (SPS/PPS, timescales)
├── scanner.py       # mdat scanner (video/audio sample detection)
├── atoms.py         # MP4 atom builders (moov, trak, stbl boxes)
├── writer.py        # Output writer + ffmpeg audio fixer
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

## Algorithm Complexity

| Stage | Time | Space |
|---|---|---|
| Reference parse | O(moov_size) | O(moov_size) |
| mdat scan | O(file_size) | O(samples) |
| AAC boundaries (per chunk) | O(n × c × log c) | O(n × c) |
| moov build | O(samples) | O(samples) |
| File write | O(file_size) | O(1) streaming |

Where n = frames per chunk (~15), c = candidates (capped at 500).

## Reference File Requirements

- Same **app** (e.g., Windows Screen Sketch)
- Same **resolution** and **frame rate**
- Same **codec** (H.264 + AAC)
- **Not corrupted** (plays normally)
- Shorter files (1-10 min) tend to have cleaner metadata
