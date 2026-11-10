"""Build Icebreaker Text 16 (OFL-1.1) from upstream Unifont 16.0.04 .hex.gz.

Usage: python tools/build_display_font.py <unifont-16.0.04.hex.gz>
Normal firmware builds use the checked-in binary and need no Python/network.
"""

import gzip
import hashlib
import sys
from pathlib import Path


RANGES = [(0x20, 0x7E), (0xB7, 0xB7), (0x2000, 0x206F), (0x25A1, 0x25A1),
          (0x3000, 0x303F), (0x4E00, 0x9FFF), (0xFF00, 0xFFEF)]
ROOT = Path(__file__).resolve().parents[1]


def main():
    source = Path(sys.argv[1]).read_bytes()
    glyphs = {}
    for line in gzip.decompress(source).decode("ascii").splitlines():
        code, bitmap = line.split(":")
        glyphs[int(code, 16)] = bytes.fromhex(bitmap)
    data = bytearray()
    subset = []
    for first, last in RANGES:
        for code in range(first, last + 1):
            bitmap = glyphs.get(code, glyphs[0x25A1])
            subset.append(f"{code:04X}:{bitmap.hex().upper()}")
            if len(bitmap) == 16:
                bitmap = b"".join(bytes([row, 0]) for row in bitmap)
            assert len(bitmap) == 32
            data.extend(bitmap)
    target = ROOT / "firmware" / "fonts"
    target.mkdir(exist_ok=True)
    (target / "icebreaker_text16.bin").write_bytes(data)
    (target / "icebreaker_text16.hex.gz").write_bytes(
        gzip.compress(("\n".join(subset) + "\n").encode("ascii"), mtime=0))
    print(f"Font: {len(data)} bytes; upstream SHA256={hashlib.sha256(source).hexdigest()}")


if __name__ == "__main__":
    main()
