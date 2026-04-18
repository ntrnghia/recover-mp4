"""Output file writer and ffmpeg audio fixer."""

import os
import re
import struct
import shutil
import subprocess


def write_output(corrupted_path, output_path, scan_info, moov):
    """Write recovered MP4: copy pre-mdat + mdat data + append moov."""
    mdat_offset = scan_info['mdat_offset']
    mdat_size = scan_info['mdat_size']

    with open(corrupted_path, 'rb') as src, open(output_path, 'wb') as dst:
        # Copy pre-mdat boxes (ftyp, uuid, etc.)
        src.seek(0)
        dst.write(src.read(mdat_offset))

        # Write mdat with correct size header
        if mdat_size > 0xFFFFFFFF:
            dst.write(struct.pack('>I4sQ', 1, b'mdat', mdat_size))
            src.seek(mdat_offset + 8)
            remaining = mdat_size - 16
        else:
            dst.write(struct.pack('>I4s', mdat_size, b'mdat'))
            src.seek(mdat_offset + 8)
            remaining = mdat_size - 8

        # Stream mdat content
        BUF = 4 * 1024 * 1024
        while remaining > 0:
            chunk = src.read(min(BUF, remaining))
            if not chunk:
                break
            dst.write(chunk)
            remaining -= len(chunk)

        # Append moov
        dst.write(moov)


def fix_audio(output_path):
    """Re-encode audio with ffmpeg to fix frame boundary errors.

    Raw AAC frames in mdat lack self-delimiting markers, so frame boundary
    detection is imprecise. Re-encoding audio fixes decode errors while
    keeping the perfect video stream untouched.

    Strategy: extract video (untouched) and re-encode audio separately,
    then merge. If audio re-encode crashes, use segment-based processing
    on audio only (never segments the video, avoiding dts overlap issues).
    """
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print("  WARNING: ffmpeg not found - skipping audio fix.")
        print("  Run manually: ffmpeg -i OUTPUT -c:v copy -c:a aac -b:a 192k FIXED.mp4")
        return False

    tmp = output_path + '.tmp.mp4'

    # First try simple single-pass re-encode with error tolerance
    result = subprocess.run(
        [ffmpeg, '-y', '-v', 'error',
         '-err_detect', 'ignore_err', '-fflags', '+genpts+discardcorrupt',
         '-i', output_path,
         '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k',
         '-af', 'aresample=async=1', tmp],
        capture_output=True, text=True, timeout=7200)
    if result.returncode == 0:
        os.replace(tmp, output_path)
        return True
    if os.path.exists(tmp):
        os.remove(tmp)

    # Get duration for segment-based fallback
    duration = _probe_duration(ffmpeg, output_path)

    print(f"  Single-pass failed, using split-merge approach ({duration:.0f}s)...")

    base = os.path.splitext(output_path)[0]
    video_only = f"{base}_vidtmp.mp4"
    audio_fixed = f"{base}_audtmp.m4a"
    seg_files = []
    concat_list = f"{base}_concat.txt"

    def _cleanup():
        for p in [video_only, audio_fixed, tmp, concat_list]:
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError:
                pass
        for sf in seg_files:
            try:
                os.remove(sf)
            except OSError:
                pass

    try:
        # Extract video (untouched)
        r = subprocess.run(
            [ffmpeg, '-y', '-v', 'error', '-i', output_path,
             '-an', '-c:v', 'copy', video_only],
            capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            print("  WARNING: Video extraction failed.")
            return False

        # Try single-pass audio-only re-encode
        r = subprocess.run(
            [ffmpeg, '-y', '-v', 'error',
             '-err_detect', 'ignore_err', '-fflags', '+genpts+discardcorrupt',
             '-i', output_path,
             '-vn', '-c:a', 'aac', '-b:a', '192k',
             '-af', 'aresample=async=1', audio_fixed],
            capture_output=True, text=True, timeout=7200)

        if r.returncode != 0:
            # Segment-based audio-only re-encode
            if os.path.exists(audio_fixed):
                os.remove(audio_fixed)

            seg_idx = [0]
            skipped = 0.0

            def _try_seg(ss, dur, path):
                r = subprocess.run(
                    [ffmpeg, '-y', '-v', 'error',
                     '-err_detect', 'ignore_err',
                     '-fflags', '+genpts+discardcorrupt',
                     '-ss', f'{ss:.3f}', '-t', f'{dur:.3f}',
                     '-i', output_path, '-vn', '-c:a', 'aac', '-b:a', '192k',
                     '-af', 'aresample=async=1', path],
                    capture_output=True, text=True, timeout=600)
                return r.returncode == 0

            def _next_seg():
                p = f"{base}_aseg{seg_idx[0]:04d}.m4a"
                seg_idx[0] += 1
                return p

            t = 0.0
            seg_dur = 60.0
            sub_dur = 10.0

            while t < duration:
                chunk_len = min(seg_dur, duration - t)
                seg_path = _next_seg()

                if _try_seg(t, chunk_len, seg_path):
                    seg_files.append(seg_path)
                    t += chunk_len
                else:
                    if os.path.exists(seg_path):
                        os.remove(seg_path)
                    st = t
                    while st < t + chunk_len:
                        sub_len = min(sub_dur, t + chunk_len - st)
                        sub_path = _next_seg()
                        if _try_seg(st, sub_len, sub_path):
                            seg_files.append(sub_path)
                        else:
                            skipped += sub_len
                            if os.path.exists(sub_path):
                                os.remove(sub_path)
                        st += sub_len
                    t += chunk_len

            if not seg_files:
                print("  WARNING: All audio segments failed.")
                return False

            # Concat audio segments
            with open(concat_list, 'w') as cl:
                for sf in seg_files:
                    cl.write(f"file '{os.path.basename(sf)}'\n")

            r = subprocess.run(
                [ffmpeg, '-y', '-v', 'error', '-f', 'concat', '-safe', '0',
                 '-i', concat_list, '-c', 'copy', audio_fixed],
                capture_output=True, text=True, timeout=600)

            for sf in seg_files:
                try:
                    os.remove(sf)
                except OSError:
                    pass
            seg_files.clear()
            try:
                os.remove(concat_list)
            except OSError:
                pass

            if r.returncode != 0:
                print("  WARNING: Audio concat failed.")
                return False

            if skipped > 0:
                print(f"  Re-encoded audio in segments ({skipped:.0f}s gaps).")

        # Merge video + fixed audio
        r = subprocess.run(
            [ffmpeg, '-y', '-v', 'error',
             '-i', video_only, '-i', audio_fixed,
             '-c', 'copy', '-map', '0:v', '-map', '1:a', tmp],
            capture_output=True, text=True, timeout=600)

        if r.returncode == 0:
            os.replace(tmp, output_path)
            _cleanup()
            return True
        else:
            print("  WARNING: Final merge failed.")
            return False

    finally:
        _cleanup()


def _probe_duration(ffmpeg, path):
    """Probe file duration in seconds using ffmpeg."""
    probe = subprocess.run(
        [ffmpeg, '-i', path], capture_output=True, text=True, timeout=30)
    for line in (probe.stderr or '').split('\n'):
        m = re.search(r'Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)', line)
        if m:
            return (int(m.group(1)) * 3600 +
                    int(m.group(2)) * 60 +
                    float(m.group(3)))
    return 7200
