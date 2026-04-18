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

    return _parse_moov(moov_data)


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

    # Defaults
    info['video'].setdefault('timescale', 30000)
    info['video'].setdefault('sample_delta', 1000)
    info['video'].setdefault('width', 1920)
    info['video'].setdefault('height', 1080)
    info['audio'].setdefault('timescale', 48000)
    info['audio'].setdefault('sample_delta', 1024)
    info['audio'].setdefault('samples_per_chunk', 15)

    return info


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
