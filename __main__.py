"""MP4 Recovery Tool for Windows Snipping Tool recordings.

Recovers corrupted MP4 files missing the moov atom by scanning the mdat
for H.264 NAL units and AAC audio, then rebuilding the index.

Usage:
    python -m recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]

When no reference is provided, codec config is auto-detected from the
encoder SEI embedded in the corrupted file's mdat.

No external dependencies — uses only Python stdlib (+ ffmpeg for audio fix).
"""

import os
import sys

from .reference import parse_reference, detect_config
from .scanner import scan_mdat
from .atoms import build_moov
from .writer import write_output, fix_audio


def main():
    if len(sys.argv) < 2:
        print("Usage: python -m recover_mp4 <corrupted.mp4> [reference.mp4] [output.mp4]")
        sys.exit(1)

    corrupted = sys.argv[1]

    # Determine if second arg is a reference file or output path
    reference = None
    output = None
    if len(sys.argv) >= 3:
        arg2 = sys.argv[2]
        if arg2.endswith('.mp4') and os.path.exists(arg2) and len(sys.argv) >= 4:
            # 3 args: corrupted reference output
            reference = arg2
            output = sys.argv[3]
        elif arg2.endswith('.mp4') and os.path.exists(arg2):
            # Could be reference or output — check if it has moov
            reference = arg2
        else:
            output = arg2

    if output is None:
        output = corrupted.rsplit('.', 1)[0] + '_recovered.mp4'

    if not os.path.exists(corrupted):
        print(f"Error: {corrupted} not found")
        sys.exit(1)

    if reference:
        print("[1/5] Parsing reference file...")
        ref = parse_reference(reference)
    else:
        print("[1/5] Auto-detecting codec config from corrupted file...")
        ref = detect_config(corrupted)

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

    # Adjust chunk offsets if mdat header size changes in output.
    # Original file may have 8-byte header (size=0), but >4GB mdat needs
    # a 16-byte extended header in output — shifting all data offsets by +8.
    import struct as _struct
    with open(corrupted, 'rb') as _f:
        _f.seek(scan['mdat_offset'])
        _orig_hdr = 16 if _struct.unpack('>I', _f.read(4))[0] == 1 else 8
    _new_hdr = 16 if scan['mdat_size'] > 0xFFFFFFFF else 8
    _delta = _new_hdr - _orig_hdr
    if _delta:
        print(f"  mdat header: {_orig_hdr}B→{_new_hdr}B, adjusting chunk offsets by +{_delta}")
        scan['video_chunks'] = [(o + _delta, s) for o, s in scan['video_chunks']]
        scan['audio_chunks'] = [(o + _delta, s) for o, s in scan['audio_chunks']]

    print("\n[3/5] Building moov atom...")
    moov = build_moov(ref, scan)
    print(f"  moov size: {len(moov):,} bytes")

    print("\n[4/5] Writing output file...", end='', flush=True)
    import time as _time
    t_write = _time.perf_counter()
    write_output(corrupted, output, scan, moov)
    w_elapsed = _time.perf_counter() - t_write
    size_mb = os.path.getsize(output) / 1024 / 1024
    print(f" {size_mb:.0f} MB written. ({w_elapsed:.1f}s)")

    print("\n[5/5] Fixing audio...")
    fix_audio(output)

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
