"""Reference MP4 file parser — extracts codec config and timing parameters."""

import struct


def parse_reference(path):
    """Extract codec config and timing from a working reference MP4."""
    with open(path, 'rb') as f:
        f.seek(0, 2)
        file_size = f.tell()
        f.seek(0)

        moov_data = None
        while f.tell() < file_size:
            pos = f.tell()
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            size, btype = struct.unpack('>I4s', hdr)
            if size == 1:
                size = struct.unpack('>Q', f.read(8))[0]
            elif size == 0:
                size = file_size - pos
            if btype == b'moov':
                f.seek(pos)
                moov_data = f.read(size)
                break
            if size < 8:
                break
            f.seek(pos + size)

        if not moov_data:
            raise ValueError("No moov atom in reference file")

        info = _parse_moov(moov_data)
        _extract_aac_header_patterns(f, moov_data, info)

    return info


def _parse_moov(moov):
    """Parse moov data to extract all needed parameters."""
    info = {
        'video': {},
        'audio': {},
        'mvhd_timescale': 10_000_000,
    }

    # mvhd timescale
    mvhd_off = moov.find(b'mvhd')
    if mvhd_off >= 0:
        info['mvhd_timescale'] = struct.unpack('>I', moov[mvhd_off + 16:mvhd_off + 20])[0]

    # Video track
    vide_off = moov.find(b'vide')
    if vide_off >= 0:
        mdhd_off = moov.rfind(b'mdhd', 0, vide_off)
        if mdhd_off >= 0:
            info['video']['timescale'] = struct.unpack('>I', moov[mdhd_off + 16:mdhd_off + 20])[0]

        soun_off = moov.find(b'soun')
        search_end = soun_off if soun_off >= 0 else len(moov)
        stts_off = moov.find(b'stts', vide_off, search_end)
        if stts_off >= 0:
            entry_count = struct.unpack('>I', moov[stts_off + 8:stts_off + 12])[0]
            if entry_count > 0:
                _, delta = struct.unpack('>II', moov[stts_off + 12:stts_off + 20])
                info['video']['sample_delta'] = delta

        avc1_off = moov.find(b'avc1', max(0, vide_off - 200))
        if avc1_off >= 0:
            info['video']['width'] = struct.unpack('>H', moov[avc1_off + 28:avc1_off + 30])[0]
            info['video']['height'] = struct.unpack('>H', moov[avc1_off + 30:avc1_off + 32])[0]

            avcc_off = moov.find(b'avcC', avc1_off, avc1_off + 200)
            if avcc_off >= 0:
                avcc_size = struct.unpack('>I', moov[avcc_off - 4:avcc_off])[0]
                avcc = moov[avcc_off + 4:avcc_off - 4 + avcc_size]
                _parse_avcc(avcc, info)

    # Audio track
    soun_off = moov.find(b'soun')
    if soun_off >= 0:
        mdhd_off = moov.rfind(b'mdhd', 0, soun_off)
        if mdhd_off >= 0:
            info['audio']['timescale'] = struct.unpack('>I', moov[mdhd_off + 16:mdhd_off + 20])[0]

        stts_off = moov.find(b'stts', soun_off)
        if stts_off >= 0:
            entry_count = struct.unpack('>I', moov[stts_off + 8:stts_off + 12])[0]
            if entry_count > 0:
                _, delta = struct.unpack('>II', moov[stts_off + 12:stts_off + 20])
                info['audio']['sample_delta'] = delta

        stsc_off = moov.find(b'stsc', soun_off)
        if stsc_off >= 0:
            entry_count = struct.unpack('>I', moov[stsc_off + 8:stsc_off + 12])[0]
            if entry_count > 0:
                _, spc, _ = struct.unpack('>III', moov[stsc_off + 12:stsc_off + 24])
                info['audio']['samples_per_chunk'] = spc

        # Extract audio frame size stats from stsz
        stsz_off = moov.find(b'stsz', soun_off)
        if stsz_off >= 0:
            uniform = struct.unpack('>I', moov[stsz_off + 8:stsz_off + 12])[0]
            count = struct.unpack('>I', moov[stsz_off + 12:stsz_off + 16])[0]
            if uniform > 0:
                info['audio']['frame_size_min'] = uniform
                info['audio']['frame_size_max'] = uniform
                info['audio']['frame_size_mean'] = float(uniform)
                info['audio']['frame_size_stdev'] = 0.0
            elif count > 0 and stsz_off + 16 + count * 4 <= len(moov):
                sizes = struct.unpack(f'>{count}I',
                                      moov[stsz_off + 16:stsz_off + 16 + count * 4])
                info['audio']['frame_size_min'] = min(sizes)
                info['audio']['frame_size_max'] = max(sizes)
                mean = sum(sizes) / len(sizes)
                info['audio']['frame_size_mean'] = mean
                variance = sum((s - mean) ** 2 for s in sizes) / len(sizes)
                info['audio']['frame_size_stdev'] = variance ** 0.5

    # Defaults
    info['video'].setdefault('timescale', 30000)
    info['video'].setdefault('sample_delta', 1000)
    info['video'].setdefault('width', 1920)
    info['video'].setdefault('height', 1080)
    info['audio'].setdefault('timescale', 48000)
    info['audio'].setdefault('sample_delta', 1024)
    info['audio'].setdefault('samples_per_chunk', 15)

    return info


def _extract_aac_header_patterns(f, moov, info):
    """Extract dominant AAC header patterns from reference audio frames.

    Reads the first 3 bytes of each audio frame to determine the most common
    max_sfb (per window type) and ms_mask_present values. These are used by
    the scanner to strongly prefer candidates matching the reference pattern.
    """
    soun_off = moov.find(b'soun')
    if soun_off < 0:
        return

    # Get audio stsz sizes
    stsz_off = moov.find(b'stsz', soun_off)
    if stsz_off < 0:
        return
    uniform = struct.unpack('>I', moov[stsz_off + 8:stsz_off + 12])[0]
    count = struct.unpack('>I', moov[stsz_off + 12:stsz_off + 16])[0]
    if count == 0:
        return
    if uniform > 0:
        sizes = [uniform] * count
    elif stsz_off + 16 + count * 4 <= len(moov):
        sizes = list(struct.unpack(f'>{count}I',
                                   moov[stsz_off + 16:stsz_off + 16 + count * 4]))
    else:
        return

    # Get audio stco offsets
    stco_off = moov.find(b'stco', soun_off)
    if stco_off < 0:
        return
    chunk_count = struct.unpack('>I', moov[stco_off + 8:stco_off + 12])[0]
    if chunk_count == 0:
        return
    chunk_offsets = list(struct.unpack(
        f'>{chunk_count}I',
        moov[stco_off + 12:stco_off + 12 + chunk_count * 4]))

    spc = info['audio'].get('samples_per_chunk', 15)

    # Read first 3 bytes of each frame (batch-read per chunk)
    msf_long_counts = {}
    msf_short_counts = {}
    ms_counts = {}
    si = 0
    for co in chunk_offsets:
        n = min(spc, len(sizes) - si)
        chunk_total = sum(sizes[si:si + n])
        f.seek(co)
        chunk_data = f.read(chunk_total)
        off = 0
        for j in range(n):
            if off + 3 <= len(chunk_data):
                b0, b1, b2 = chunk_data[off], chunk_data[off + 1], chunk_data[off + 2]
                if b0 in (0x20, 0x21) and not (b1 & 0x80):
                    ws = (b1 >> 5) & 0x03
                    if ws == 2:
                        msf = b1 & 0x0F
                        msf_short_counts[msf] = msf_short_counts.get(msf, 0) + 1
                    else:
                        msf = ((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)
                        msf_long_counts[msf] = msf_long_counts.get(msf, 0) + 1
                    if b0 == 0x21 and ws != 2:
                        ms = (b2 >> 3) & 0x03
                        ms_counts[ms] = ms_counts.get(ms, 0) + 1
            off += sizes[si]
            si += 1

    # Store the dominant values
    if msf_long_counts:
        info['audio']['dominant_msf_long'] = max(msf_long_counts,
                                                  key=msf_long_counts.get)
    if msf_short_counts:
        info['audio']['dominant_msf_short'] = max(msf_short_counts,
                                                   key=msf_short_counts.get)
    if ms_counts:
        info['audio']['dominant_ms'] = max(ms_counts, key=ms_counts.get)


def _parse_avcc(data, info):
    """Parse avcC content to extract SPS and PPS."""
    if len(data) < 8:
        return

    num_sps = data[5] & 0x1F
    pos = 6
    if num_sps > 0 and pos + 2 <= len(data):
        sps_len = struct.unpack('>H', data[pos:pos + 2])[0]
        pos += 2
        if pos + sps_len <= len(data):
            info['video']['sps'] = data[pos:pos + sps_len]
            pos += sps_len

    if pos < len(data):
        num_pps = data[pos]
        pos += 1
        if num_pps > 0 and pos + 2 <= len(data):
            pps_len = struct.unpack('>H', data[pos:pos + 2])[0]
            pos += 2
            if pos + pps_len <= len(data):
                info['video']['pps'] = data[pos:pos + pps_len]
