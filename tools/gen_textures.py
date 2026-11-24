#!/usr/bin/env python3
"""World surface textures (Phase 11): ground, dungeon, water and cloud maps, all generated here.

Every texture tiles seamlessly (the noise and the cell patterns wrap at the edges) and is original
to the project, released CC0 (see ASSETS.md); no third-party input.

Ground textures hold only fine detail (16 or more noise cells per tile): anything coarser shows as
a grid when a field repeats the tile hundreds of times. The large-scale variation comes from the
macro map, which the terrain shader samples at two much larger scales.

Ground and dungeon textures are *detail* maps: values around 0.5 that the shaders read as a factor
(value / 0.5) on a colour from the content (a zone's theme, a biome's layer tint). So one grass map
serves the meadow, the steppe and the bog, each in its own colour, and the low-poly palette stays
the content's. Colour in a detail map is only a slight shift around grey.

  python3 tools/gen_textures.py          write client/assets/textures/*.png
  python3 tools/gen_textures.py --check  exit 1 if a committed texture is missing or stale

Needs numpy and Pillow (tools/requirements.txt; on Debian/Ubuntu: python3-numpy python3-pil). The
check compares pixels with a small tolerance, not file bytes: PNG encoders and floating-point
libraries differ a little between machines.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "client" / "assets" / "textures"
SIZE = 512


# --- tileable noise -------------------------------------------------------------------------------

def value_noise(size: int, period: int, rng: np.random.Generator) -> np.ndarray:
    """Smooth value noise with `period` lattice cells across the image; wraps at the edges."""
    lattice = rng.random((period, period))
    coords = np.arange(size) * period / size
    i0 = np.floor(coords).astype(int)
    f = coords - i0
    f = f * f * f * (f * (f * 6 - 15) + 10)  # quintic fade
    i1 = (i0 + 1) % period
    i0 = i0 % period
    fy = f[:, None]
    fx = f[None, :]
    a = lattice[np.ix_(i0, i0)]
    b = lattice[np.ix_(i0, i1)]
    c = lattice[np.ix_(i1, i0)]
    d = lattice[np.ix_(i1, i1)]
    top = a + (b - a) * fx
    bottom = c + (d - c) * fx
    return top + (bottom - top) * fy


def fbm(size: int, base: int, octaves: int, rng: np.random.Generator, gain: float = 0.5) -> np.ndarray:
    """Fractal noise in 0..1 from `octaves` layers of value noise, starting at `base` cells."""
    total = np.zeros((size, size))
    amp = 1.0
    norm = 0.0
    period = base
    for _ in range(octaves):
        total += value_noise(size, period, rng) * amp
        norm += amp
        amp *= gain
        period *= 2
    return total / norm


def ridged(size: int, base: int, octaves: int, rng: np.random.Generator) -> np.ndarray:
    """Ridged noise: sharp creases where the noise crosses its middle (rock strata, cracks)."""
    total = np.zeros((size, size))
    amp = 1.0
    norm = 0.0
    period = base
    for _ in range(octaves):
        n = 1.0 - np.abs(value_noise(size, period, rng) * 2.0 - 1.0)
        total += n * n * amp
        norm += amp
        amp *= 0.5
        period *= 2
    return total / norm


def cells(size: int, count: int, rng: np.random.Generator, jitter: float = 0.9):
    """Worley cells on a count x count jittered grid, wrapping: (F1, F2, id of the nearest cell)."""
    pts = (np.stack(np.meshgrid(np.arange(count), np.arange(count), indexing="ij"), -1)
           + 0.5 + (rng.random((count, count, 2)) - 0.5) * jitter)
    coords = (np.arange(size) + 0.5) * count / size
    py = coords[:, None]
    px = coords[None, :]
    cy = np.floor(py).astype(int)
    cx = np.floor(px).astype(int)
    f1 = np.full((size, size), 9.0)
    f2 = np.full((size, size), 9.0)
    ident = np.zeros((size, size), dtype=int)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            gy = cy + dy
            gx = cx + dx
            wy = gy % count
            wx = gx % count
            p = pts[wy, wx]  # broadcast (size, size, 2)
            oy = p[..., 0] + (gy - wy) - py
            ox = p[..., 1] + (gx - wx) - px
            d = np.sqrt(oy * oy + ox * ox)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            ident = np.where(closer, wy * count + wx, ident)
            f1 = np.where(closer, d, f1)
    return f1, f2, ident


def bond(size: int, rows: int, cols: int, mortar: float, rng: np.random.Generator,
         stagger: float = 0.5, wobble: float = 0.0):
    """Running-bond masonry: (edge distance 0..1 inside each unit, unit id). Wraps at the edges."""
    y = (np.arange(size) + 0.5) / size * rows
    x = (np.arange(size) + 0.5) / size * cols
    row = np.floor(y).astype(int)
    shift = (row % 2) * stagger + (rng.random(rows)[row] - 0.5) * wobble
    yy = np.broadcast_to(y[:, None], (size, size))
    xx = x[None, :] + shift[:, None]
    col = np.floor(xx).astype(int) % cols
    fy = yy - np.floor(yy)
    fx = xx - np.floor(xx)
    # Distance to the nearest joint in units of the mortar width (aspect corrected).
    aspect = (size / rows) / (size / cols)
    edge = np.minimum(np.minimum(fy, 1 - fy), np.minimum(fx, 1 - fx) / aspect)
    inside = np.clip((edge - mortar) / max(mortar, 1e-6), 0.0, 1.0)
    ident = np.broadcast_to(row[:, None], (size, size)) * cols + col
    return inside, ident


# --- helpers --------------------------------------------------------------------------------------

def normalize(v: np.ndarray, mean: float = 0.5, spread: float = 0.18) -> np.ndarray:
    """Recentre a map to `mean` with standard deviation `spread`, clipped to 0..1."""
    s = v.std()
    out = (v - v.mean()) / (s if s > 1e-9 else 1.0) * spread + mean
    return np.clip(out, 0.0, 1.0)


def tinted(v: np.ndarray, hue: np.ndarray, r: float, g: float, b: float) -> np.ndarray:
    """A grey detail map with a slight colour shift: hue in -1..1 pushes along (r, g, b)."""
    return np.clip(np.stack([v * (1 + hue * r), v * (1 + hue * g), v * (1 + hue * b)], -1), 0.0, 1.0)


def grey(v: np.ndarray) -> np.ndarray:
    return np.stack([v, v, v], -1)


def per_id(ident: np.ndarray, count: int, rng: np.random.Generator) -> np.ndarray:
    return rng.random(count)[ident]


# --- the textures ---------------------------------------------------------------------------------

def grass(rng):
    clumps = fbm(SIZE, 16, 4, rng)
    blades = fbm(SIZE, 128, 2, rng)
    speck = value_noise(SIZE, 256, rng)
    v = 0.4 * clumps + 0.4 * blades + 0.2 * speck
    dry = np.clip(fbm(SIZE, 16, 3, rng) * 2 - 0.8, 0.0, 1.0)  # dry tufts lean straw-coloured
    return tinted(normalize(v, 0.5, 0.13), dry, 0.08, 0.03, -0.1)


def dirt(rng):
    v = 0.6 * fbm(SIZE, 16, 4, rng) + 0.25 * value_noise(SIZE, 128, rng)
    f1, _, ident = cells(SIZE, 40, rng)
    pebble = np.clip(1.0 - f1 / 0.32, 0.0, 1.0) * (per_id(ident, 1600, rng) > 0.72)
    v = v + pebble * 0.2
    hue = fbm(SIZE, 16, 2, rng)
    return tinted(normalize(v, 0.5, 0.1), hue, 0.05, 0.0, -0.05)


def rock(rng):
    strata = ridged(SIZE, 8, 5, rng)
    f1, f2, _ = cells(SIZE, 10, rng)
    crack = np.clip((f2 - f1) / 0.05, 0.0, 1.0)
    v = 0.6 * strata + 0.35 * fbm(SIZE, 16, 4, rng) + 0.05 * crack
    v = v * (0.88 + 0.12 * crack)
    return grey(normalize(v, 0.5, 0.17))


def sand(rng):
    warp = fbm(SIZE, 8, 3, rng)
    y = np.arange(SIZE)[:, None] / SIZE
    ripples = 0.5 + 0.5 * np.sin(2 * np.pi * (24 * y + 1.5 * warp))
    grain = value_noise(SIZE, 256, rng)
    v = 0.15 * ripples + 0.35 * fbm(SIZE, 16, 4, rng) + 0.5 * grain
    hue = fbm(SIZE, 16, 2, rng)
    return tinted(normalize(v, 0.5, 0.09), hue, 0.04, 0.02, -0.04)


def snow(rng):
    drifts = fbm(SIZE, 16, 4, rng)
    warp = fbm(SIZE, 8, 2, rng)
    x = np.arange(SIZE)[None, :] / SIZE
    sastrugi = 0.5 + 0.5 * np.sin(2 * np.pi * (16 * x + 1.0 * warp))
    sparkle = (value_noise(SIZE, 256, rng) > 0.93) * 0.25
    v = 0.7 * drifts + 0.1 * sastrugi + sparkle
    # Hollows lean blue, as snow does under a clear sky.
    return tinted(normalize(v, 0.5, 0.08), np.clip(1.0 - drifts * 2, 0.0, 1.0), -0.05, 0.0, 0.05)


def mud(rng):
    base = fbm(SIZE, 16, 4, rng)
    wet = fbm(SIZE, 16, 4, rng)
    pools = np.clip((wet - 0.5) / 0.25, 0.0, 1.0)
    v = base * (1.0 - 0.35 * pools) + 0.2 * value_noise(SIZE, 96, rng) * (1.0 - pools)
    return tinted(normalize(v, 0.5, 0.14), pools, -0.03, 0.0, 0.03)


def ash(rng):
    f1, f2, ident = cells(SIZE, 24, rng)
    flakes = np.clip((f2 - f1) / 0.12, 0.0, 1.0) * (0.6 + 0.4 * per_id(ident, 576, rng))
    v = 0.5 * fbm(SIZE, 16, 4, rng) + 0.35 * flakes + 0.15 * value_noise(SIZE, 192, rng)
    return grey(normalize(v, 0.5, 0.14))


def cobble(rng):
    f1, f2, ident = cells(SIZE, 12, rng, jitter=0.7)
    stone = np.clip((f2 - f1) / 0.14, 0.0, 1.0)
    dome = np.clip(1.0 - f1 / 0.75, 0.0, 1.0)
    shade = 0.75 + 0.5 * per_id(ident, 144, rng)
    v = stone * (0.55 + 0.3 * dome) * shade + 0.15 * fbm(SIZE, 32, 3, rng)
    return grey(normalize(v, 0.5, 0.2))


def masonry(rng, rows, cols, mortar, wobble, rough, spread):
    inside, ident = bond(SIZE, rows, cols, mortar, rng, wobble=wobble)
    shade = 0.8 + 0.4 * per_id(ident, rows * cols, rng)
    surface = fbm(SIZE, 16, 4, rng)
    chips = ridged(SIZE, 8, 3, rng)
    v = inside * shade * (1.0 - rough + rough * surface) * (0.9 + 0.1 * chips)
    v = v + (1.0 - inside) * 0.18 * surface
    return grey(normalize(v, 0.5, spread))


def flagstone(rng):
    return masonry(rng, rows=4, cols=3, mortar=0.035, wobble=0.6, rough=0.45, spread=0.18)


def brick(rng):
    return masonry(rng, rows=8, cols=4, mortar=0.06, wobble=0.0, rough=0.35, spread=0.2)


def blocks(rng):
    return masonry(rng, rows=4, cols=2, mortar=0.03, wobble=0.0, rough=0.4, spread=0.16)


def tiles(rng):
    inside, ident = bond(SIZE, 4, 4, 0.025, rng, stagger=0.0)
    shade = 0.9 + 0.2 * per_id(ident, 16, rng)
    v = inside * shade * (0.85 + 0.15 * fbm(SIZE, 8, 4, rng)) + (1.0 - inside) * 0.25
    return grey(normalize(v, 0.5, 0.15))


def cave_rock(rng):
    v = 0.5 * ridged(SIZE, 6, 5, rng) + 0.35 * fbm(SIZE, 4, 5, rng)
    f1, f2, _ = cells(SIZE, 7, rng)
    v = v * (0.7 + 0.3 * np.clip((f2 - f1) / 0.1, 0.0, 1.0))
    return grey(normalize(v, 0.5, 0.18))


def scorched(rng):
    f1, f2, _ = cells(SIZE, 9, rng)
    crack = 1.0 - np.clip((f2 - f1) / 0.06, 0.0, 1.0)
    v = 0.6 * fbm(SIZE, 6, 5, rng) + 0.4 * ridged(SIZE, 12, 3, rng)
    v = v * (1.0 - 0.7 * crack)
    # The cracks glow a little: warm hue where they are.
    return tinted(normalize(v, 0.5, 0.17), crack, 0.6, 0.0, -0.4)


def macro(rng):
    """Very low-frequency variation; the terrain shader samples it at a large scale to hide tiling."""
    a = fbm(SIZE, 2, 6, rng)
    b = fbm(SIZE, 3, 5, rng)
    c = fbm(SIZE, 5, 4, rng)
    return np.stack([normalize(a, 0.5, 0.2), normalize(b, 0.5, 0.2), normalize(c, 0.5, 0.2)], -1)


def clouds(rng):
    """Cloud density in red (puffy), green (wispy) and blue (high haze); the sky shader mixes them."""
    puffy = fbm(SIZE, 4, 6, rng, gain=0.55)
    wisps = fbm(SIZE, 3, 6, rng, gain=0.6)
    y = np.arange(SIZE)[:, None] / SIZE
    streak = 0.5 + 0.5 * np.sin(2 * np.pi * (3 * y + 1.5 * wisps))
    haze = fbm(SIZE, 2, 4, rng)
    return np.stack([normalize(puffy, 0.5, 0.2), normalize(wisps * streak, 0.5, 0.2), normalize(haze, 0.5, 0.2)], -1)


def water_normal(rng):
    """A tangent-space normal map of small waves, from a fractal height field."""
    h = fbm(SIZE, 8, 5, rng, gain=0.55) * 0.6 + ridged(SIZE, 4, 3, rng) * 0.4
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * SIZE / 16.0
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * SIZE / 16.0
    n = np.stack([-dx, -dy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


TEXTURES = {
    "grass": grass, "dirt": dirt, "rock": rock, "sand": sand, "snow": snow, "mud": mud,
    "ash": ash, "cobble": cobble, "flagstone": flagstone, "brick": brick, "blocks": blocks,
    "tiles": tiles, "cave_rock": cave_rock, "scorched": scorched,
    "macro": macro, "clouds": clouds, "water_normal": water_normal,
}


def render(name: str) -> np.ndarray:
    # A seed per texture from its name, so adding one never changes another.
    seed = sum((i + 1) * ord(ch) for i, ch in enumerate(name))
    img = TEXTURES[name](np.random.default_rng(seed))
    return (np.clip(img, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def main() -> int:
    check = "--check" in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    stale = []
    for name in TEXTURES:
        pixels = render(name)
        path = OUT / f"{name}.png"
        if check:
            if not path.exists():
                stale.append(f"{name}.png (missing)")
                continue
            old = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
            diff = np.abs(old - pixels.astype(np.int16)) if old.shape == pixels.shape else None
            if diff is None or diff.max() > 3 or diff.mean() > 0.05:
                stale.append(f"{name}.png")
            continue
        Image.fromarray(pixels, "RGB").save(path, optimize=True)
    known = {f"{n}.png" for n in TEXTURES}
    extra = sorted(p.name for p in OUT.glob("*.png") if p.name not in known)
    if check:
        if stale or extra:
            for s in stale:
                print(f"stale: client/assets/textures/{s}")
            for e in extra:
                print(f"not made by this script: client/assets/textures/{e}")
            print("run: python3 tools/gen_textures.py")
            return 1
        print(f"textures: {len(TEXTURES)} up to date")
        return 0
    print(f"wrote {len(TEXTURES)} textures to {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
