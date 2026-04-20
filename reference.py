"""Reference MP4 file parser — extracts codec config and timing parameters.

Also supports reference-free mode: detect_config() parses the encoder SEI
embedded in the mdat to extract resolution and encoder variant, then
constructs SPS/PPS from scratch.
"""

import math
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


# ─── Reference-free mode ────────────────────────────────────────────────────

def detect_config(path):
    """Detect codec config from corrupted file's mdat (no reference needed).

    Parses the encoder SEI NAL embedded by Microsoft H.264 Encoder V1.5.3
    to extract width, height, and cabac flag, then constructs SPS/PPS.

    Returns the same dict structure as parse_reference().
    """
    with open(path, 'rb') as f:
        f.seek(0, 2)
        file_size = f.tell()
        f.seek(0)

        # Locate mdat
        mdat_offset = None
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
            if btype == b'mdat':
                mdat_offset = pos
                break
            if size < 8:
                break
            f.seek(pos + size)

        if mdat_offset is None:
            raise ValueError("No mdat atom found")

        # Read first 4KB of mdat data (SEI is in the first access unit)
        f.seek(mdat_offset + 8)
        head = f.read(4096)

    # Parse SEI params from mdat head
    sei_params = _parse_sei_params(head)

    if sei_params:
        w = sei_params['w']
        h = sei_params['h']
        cabac = sei_params['cabac']
        print(f"  SEI detected: {w}x{h}, cabac={cabac}")
    else:
        raise ValueError(
            "No encoder SEI found in mdat. This file may use Variant B "
            "(CABAC) which doesn't embed SEI. Use a reference file or "
            "pass --resolution WxH.")

    sps = _build_sps(w, h, cabac)
    pps = _build_pps(cabac)

    return {
        'video': {
            'width': w,
            'height': h,
            'timescale': 30000,
            'sample_delta': 1000,
            'sps': sps,
            'pps': pps,
        },
        'audio': {
            'timescale': 48000,
            'sample_delta': 1024,
            'samples_per_chunk': 15,
        },
        'mvhd_timescale': 10_000_000,
    }


def _parse_sei_params(data):
    """Parse encoder SEI from the start of mdat data.

    Scans for SEI NAL units (type 6) containing user_data_unregistered
    (payload type 5) with the encoder parameter string.
    Returns dict with w, h, cabac, ref or None.
    """
    # Find first AUD NAL (marks start of video data after any preamble)
    aud_pattern = b'\x00\x00\x00\x02\x09'
    aud_idx = data.find(aud_pattern)
    if aud_idx < 0:
        return None

    pos = aud_idx
    while pos + 9 < len(data):
        # Read NAL length prefix
        nal_len = struct.unpack('>I', data[pos:pos + 4])[0]
        if nal_len < 1 or nal_len > 4096 or pos + 4 + nal_len > len(data):
            break
        nal_byte = data[pos + 4]
        nal_type = nal_byte & 0x1F

        if nal_type == 9:  # AUD — skip
            pos += 4 + nal_len
            continue

        if nal_type == 6:  # SEI
            sei_data = data[pos + 5:pos + 4 + nal_len]
            result = _parse_sei_payload(sei_data)
            if result:
                return result
            pos += 4 + nal_len
            continue

        # Hit a slice NAL — stop searching
        break

    return None


def _parse_sei_payload(sei_data):
    """Parse SEI message payloads looking for encoder params string."""
    pos = 0
    while pos < len(sei_data) - 1:
        # Read payload type
        pt = 0
        while pos < len(sei_data) and sei_data[pos] == 0xFF:
            pt += 255
            pos += 1
        if pos >= len(sei_data):
            break
        pt += sei_data[pos]
        pos += 1

        # Read payload size
        ps = 0
        while pos < len(sei_data) and sei_data[pos] == 0xFF:
            ps += 255
            pos += 1
        if pos >= len(sei_data):
            break
        ps += sei_data[pos]
        pos += 1

        if pt == 5 and ps >= 20:  # user_data_unregistered
            payload = sei_data[pos:pos + ps]
            # Skip 16-byte UUID, look for ASCII params
            text = payload[16:]
            try:
                text_str = text.decode('ascii', errors='ignore')
            except Exception:
                text_str = ''
            if 'w:' in text_str and 'h:' in text_str:
                return _parse_param_string(text_str)

        pos += ps

    return None


def _parse_param_string(text):
    """Parse 'src:3 h:1440 w:2560 ... cabac:0 ...' into dict."""
    params = {}
    for token in text.split():
        if ':' in token:
            key, _, val = token.partition(':')
            params[key] = val

    w = int(params.get('w', 0))
    h = int(params.get('h', 0))
    cabac = int(params.get('cabac', 0))

    if w == 0 or h == 0:
        return None

    return {'w': w, 'h': h, 'cabac': cabac, 'ref': int(params.get('ref', 2))}


# ─── SPS / PPS construction ─────────────────────────────────────────────────

class _BitWriter:
    __slots__ = ('data', 'byte', 'bit_pos')

    def __init__(self):
        self.data = bytearray()
        self.byte = 0
        self.bit_pos = 7

    def write_u(self, val, n):
        for i in range(n - 1, -1, -1):
            self.byte |= ((val >> i) & 1) << self.bit_pos
            self.bit_pos -= 1
            if self.bit_pos < 0:
                self.data.append(self.byte)
                self.byte = 0
                self.bit_pos = 7

    def write_ue(self, val):
        val += 1
        nbits = val.bit_length()
        self.write_u(0, nbits - 1)
        self.write_u(val, nbits)

    def flush(self):
        if self.bit_pos < 7:
            self.byte |= 1 << self.bit_pos
            self.data.append(self.byte)
        return bytes(self.data)


def _add_emulation_prevention(rbsp):
    out = bytearray()
    count = 0
    for b in rbsp:
        if count >= 2 and b <= 3:
            out.append(3)
            count = 0
        out.append(b)
        count = count + 1 if b == 0 else 0
    return bytes(out)


def _build_sps(w, h, cabac):
    """Construct SPS NAL for Microsoft H.264 Encoder V1.5.3."""
    ref = 1 if cabac else 2

    bw = _BitWriter()

    # NAL header
    bw.write_u(0, 1)   # forbidden_zero_bit
    bw.write_u(3, 2)   # nal_ref_idc = 3
    bw.write_u(7, 5)   # nal_unit_type = 7 (SPS)

    # profile_idc = 77 (Main)
    bw.write_u(77, 8)
    bw.write_u(0, 1)   # constraint_set0
    bw.write_u(1, 1)   # constraint_set1
    bw.write_u(0, 1)   # constraint_set2
    bw.write_u(0, 1)   # constraint_set3
    bw.write_u(0, 4)   # reserved

    # level_idc
    mb_count = math.ceil(w / 16) * math.ceil(h / 16)
    level = 50 if mb_count * 30 > 245760 else 40
    bw.write_u(level, 8)

    bw.write_ue(0)      # seq_parameter_set_id

    bw.write_ue(4)      # log2_max_frame_num_minus4

    if cabac:
        bw.write_ue(2)  # pic_order_cnt_type = 2
    else:
        bw.write_ue(0)  # pic_order_cnt_type = 0
        bw.write_ue(4)  # log2_max_pic_order_cnt_lsb_minus4

    bw.write_ue(ref)    # max_num_ref_frames
    bw.write_u(0, 1)    # gaps_in_frame_num_allowed

    w_mbs = math.ceil(w / 16)
    h_mbs = math.ceil(h / 16)
    bw.write_ue(w_mbs - 1)  # pic_width_in_mbs_minus1
    bw.write_ue(h_mbs - 1)  # pic_height_in_map_units_minus1

    bw.write_u(1, 1)    # frame_mbs_only_flag
    bw.write_u(1, 1)    # direct_8x8_inference_flag

    # Cropping
    crop_right = w_mbs * 16 - w
    crop_bottom = h_mbs * 16 - h
    if crop_right > 0 or crop_bottom > 0:
        bw.write_u(1, 1)
        bw.write_ue(0)
        bw.write_ue(crop_right // 2)
        bw.write_ue(0)
        bw.write_ue(crop_bottom // 2)
    else:
        bw.write_u(0, 1)

    # VUI
    bw.write_u(1, 1)    # vui_parameters_present
    bw.write_u(1, 1)    # aspect_ratio_info_present
    bw.write_u(1, 8)    # aspect_ratio_idc = 1 (square)
    bw.write_u(0, 1)    # overscan_info_present
    bw.write_u(0, 1)    # video_signal_type_present
    bw.write_u(0, 1)    # chroma_loc_info_present
    bw.write_u(1, 1)    # timing_info_present
    if cabac:
        bw.write_u(1000, 32)   # num_units_in_tick
        bw.write_u(60000, 32)  # time_scale
    else:
        bw.write_u(1, 32)      # num_units_in_tick
        bw.write_u(60, 32)     # time_scale
    bw.write_u(0, 1)    # fixed_frame_rate_flag
    bw.write_u(0, 1)    # nal_hrd_parameters_present
    bw.write_u(0, 1)    # vcl_hrd_parameters_present
    bw.write_u(0, 1)    # pic_struct_present
    bw.write_u(1, 1)    # bitstream_restriction_flag
    bw.write_u(1, 1)    # motion_vectors_over_pic_boundaries
    if cabac:
        bw.write_ue(0)   # max_bytes_per_pic_denom
        bw.write_ue(0)   # max_bits_per_mb_denom
        bw.write_ue(13)  # log2_max_mv_length_horizontal
        bw.write_ue(9)   # log2_max_mv_length_vertical
        bw.write_ue(0)   # max_num_reorder_frames
        bw.write_ue(1)   # max_dec_frame_buffering
    else:
        bw.write_ue(2)   # max_bytes_per_pic_denom
        bw.write_ue(1)   # max_bits_per_mb_denom
        bw.write_ue(16)  # log2_max_mv_length_horizontal
        bw.write_ue(16)  # log2_max_mv_length_vertical
        bw.write_ue(1)   # max_num_reorder_frames
        bw.write_ue(2)   # max_dec_frame_buffering

    rbsp = bw.flush()
    return _add_emulation_prevention(rbsp)


def _build_pps(cabac):
    """Return PPS NAL bytes for the given encoder variant."""
    if cabac:
        return bytes.fromhex('68EE3C80')
    else:
        return bytes.fromhex('68CE3C80')
