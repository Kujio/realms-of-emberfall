#!/usr/bin/env python3
"""Generate client/icon.ico (placeholder ember icon) with no third-party dependencies.

The ICO holds PNG-compressed images at 16-256 px, which Windows Vista+ supports.
"""
from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "client" / "icon.ico"


def pixel(x: float, y: float) -> tuple[int, int, int, int]:
    """x, y in [0, 1]. Dark rounded tile with a two-tone teardrop flame."""
    # Rounded-square mask.
    r = 0.18
    dx = max(abs(x - 0.5) - (0.5 - r), 0.0)
    dy = max(abs(y - 0.5) - (0.5 - r), 0.0)
    if math.hypot(dx, dy) > r:
        return (0, 0, 0, 0)

    def flame(scale: float, cy: float) -> bool:
        # Teardrop: circle at the bottom, tapering to a point at the top.
        px, py = (x - 0.5) / scale, (y - cy) / scale
        if py >= 0:
            return px * px + py * py <= 0.09
        t = -py / 0.42
        if t > 1:
            return False
        half = 0.3 * (1 - t) ** 1.4 * (1 + 0.15 * math.sin(t * 9))
        return abs(px + 0.05 * math.sin(t * 5)) <= half

    if flame(0.55, 0.66):
        return (255, 194, 74, 255)
    if flame(1.0, 0.62):
        return (232, 100, 44, 255)
    return (28, 20, 24, 255)


def png(size: int) -> bytes:
    rows = []
    ss = 3  # supersampling for smooth edges
    for j in range(size):
        row = bytearray([0])
        for i in range(size):
            acc = [0, 0, 0, 0]
            for sj in range(ss):
                for si in range(ss):
                    p = pixel((i + (si + 0.5) / ss) / size, (j + (sj + 0.5) / ss) / size)
                    for k in range(4):
                        acc[k] += p[k]
            row += bytes(v // (ss * ss) for v in acc)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main() -> None:
    sizes = [16, 32, 48, 64, 128, 256]
    images = [png(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for s, data in zip(sizes, images):
        dim = 0 if s == 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    OUT.write_bytes(header + entries + b"".join(images))
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
