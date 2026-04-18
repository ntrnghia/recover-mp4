"""Shared constants and low-level helpers for MP4 recovery."""

# 4-byte length prefix (=2) + NAL type 9 (Access Unit Delimiter)
AUD_PATTERN = b'\x00\x00\x00\x02\x09'

# Bitmask for O(1) NAL type validation (single bitshift vs hash lookup)
# Valid types: 1-12 for length-prefixed MP4 NAL units
VALID_NAL_MASK = 0x1FFE  # bits 1..12 set

MAX_NAL_SIZE = 8_000_000  # 8 MB max for a single NAL unit


def parse_slice_type(data):
    """Parse slice_type from first 4 bytes of slice header payload.

    Inlined exp-Golomb decoding — no nested function/closure overhead.
    Returns: 0/5=P, 1/6=B, 2/7=I, or -1 on error.
    """
    bits = int.from_bytes(data, 'big')
    bp = 31
    # Skip first_mb_in_slice (exp-Golomb, value discarded)
    z = 0
    while bp >= 0 and not (bits & (1 << bp)):
        z += 1
        bp -= 1
        if z > 16:
            return -1
    if bp < 0:
        return -1
    bp -= 1 + z  # skip stop-bit + z value bits
    if bp < 0:
        return -1
    # Read slice_type (exp-Golomb)
    z = 0
    while bp >= 0 and not (bits & (1 << bp)):
        z += 1
        bp -= 1
        if z > 16:
            return -1
    if bp < 0:
        return -1
    bp -= 1  # skip stop-bit
    val = 0
    for _ in range(z):
        if bp < 0:
            return -1
        val = (val << 1) | ((bits >> bp) & 1)
        bp -= 1
    return (1 << z) - 1 + val
