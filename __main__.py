"""MP4 Recovery Tool for Windows Snipping Tool recordings.

Recovers corrupted MP4 files missing the moov atom by scanning the mdat
for H.264 NAL units and AAC audio, then rebuilding the index.

Usage:
    python -m recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]

No external dependencies — uses only Python stdlib (+ ffmpeg for audio fix).
"""

import os
import sys

from .reference import parse_reference
from .scanner import scan_mdat
from .atoms import build_moov
from .writer import write_output, fix_audio


def main():
    if len(sys.argv) < 3:
        print("Usage: python -m recover_mp4 <corrupted.mp4> <reference.mp4> [output.mp4]")
        sys.exit(1)

    corrupted = sys.argv[1]
    reference = sys.argv[2]
    output = sys.argv[3] if len(sys.argv) > 3 else corrupted.rsplit('.', 1)[0] + '_recovered.mp4'

    if not os.path.exists(corrupted):
        print(f"Error: {corrupted} not found")
        sys.exit(1)
    if not os.path.exists(reference):
        print(f"Error: {reference} not found")
        sys.exit(1)

    print("[1/5] Parsing reference file...")
    ref = parse_reference(reference)
    print(f"  Video: {ref['video']['width']}x{ref['video']['height']}")
    sps = ref['video'].get('sps', b'')
    pps = ref['video'].get('pps', b'')
    print(f"  SPS: {len(sps)} bytes, PPS: {len(pps)} bytes")
    v = ref['video']
    print(f"  Video: timescale={v['timescale']}, delta={v['sample_delta']} "
          f"({v['timescale'] / v['sample_delta']:.1f} fps)")
    a = ref['audio']
    print(f"  Audio: timescale={a['timescale']}, delta={a['sample_delta']}, "
          f"samples_per_chunk={a['samples_per_chunk']}")
    print(f"  mvhd timescale: {ref['mvhd_timescale']}")

    print("\n[2/5] Scanning corrupted file...")
    scan = scan_mdat(corrupted, ref)

    print("\n[3/5] Building moov atom...")
    moov = build_moov(ref, scan)
    print(f"  moov size: {len(moov):,} bytes")

    print("\n[4/5] Writing output file...")
    write_output(corrupted, output, scan, moov)

    print("\n[5/5] Fixing audio (ffmpeg re-encode)...")
    ok = fix_audio(output)
    if ok:
        print("  Audio fixed successfully.")

    vs = len(scan['video_samples'])
    aus = len(scan['audio_samples'])
    kf = len(scan['sync_samples'])
    dur_s = vs * ref['video']['sample_delta'] / ref['video']['timescale']
    dur_m, dur_sec = divmod(int(dur_s), 60)
    size_mb = os.path.getsize(output) / 1024 / 1024

    print(f"\n{'=' * 60}")
    print(f"Recovery complete: {output}")
    print(f"  Output size:   {size_mb:.1f} MB")
    print(f"  Video samples: {vs:,}")
    print(f"  Audio samples: {aus:,}")
    print(f"  Keyframes:     {kf:,}")
    print(f"  Duration:      {dur_m}m {dur_sec}s")


if __name__ == '__main__':
    main()
