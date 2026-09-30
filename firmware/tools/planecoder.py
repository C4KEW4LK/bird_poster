"""The coder for a plate's two planes: an adaptive context model driving a
byte-wise range coder, in place of zlib.

    .venv-rembg/bin/python firmware/tools/planecoder.py --train   # rewrite the luma prior

A plane is coded one symbol at a time, each with the odds of a model picked by
what the decoder already has - the neighbours - so a pixel costs about what it
surprises. zlib looks for repeated strings, and a lithograph's shading never
repeats exactly; it is always close to the pixel beside it, which is what a
context says. Measured against zlib over the au plates: the luma plane 65%
of the bytes, the chroma plane (2x2 blocks) 76%.

Luma: one model a context of (up, left, the slope of the row above), 1280 in
all, each starting from odds learned across the plates (`PRIOR_HEADER`, which
the firmware compiles in). A sprite has too few pixels to learn 1280 contexts
from nothing; started from the prior, the richer context wins.

Chroma: one model a context of (up block, left block, the block's luma in four
bands), 1156 in all, starting flat - a plate's chroma codes are its own
palette's order, so nothing learned elsewhere applies. Blocks with nothing
painted in them are not coded: the decoder has the luma plane first and knows
them already.

The range coder is LZMA's (32-bit range, carry through a cached byte). Every
count is an integer and every step is one `lib/plates/planecoder.cpp` repeats
exactly: a model's counts start at the prior (or 1), a coded symbol gains
INC, and all are halved (kept above 0) once the total passes LIMIT. The
decoder reads exactly the bytes the encoder wrote, which makes a stream's
length a check on it.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

import numpy as np

SYMBOLS = 16
OUTSIDE = 15  # the luma code that is not a level
INC = 24
LIMIT = 1 << 14
TOP = 1 << 24

LUMA_CONTEXTS = 256 * 5
CHROMA_EDGE = 16  # a chroma neighbour that is off the sprite or unpainted
CHROMA_CONTEXTS = 17 * 17 * 4
PRIOR_STRENGTH = 1024  # what a prior row sums to: how long the model trusts it

PRIOR_HEADER = Path(__file__).resolve().parents[1] / "lib" / "plates" / "plane_prior.h"


class _Model:
    __slots__ = ("freq", "total")

    def __init__(self, freq: list[int]) -> None:
        self.freq = freq
        self.total = sum(freq)


def _update(m: _Model, s: int) -> None:
    f = m.freq
    f[s] += INC
    m.total += INC
    if m.total > LIMIT:
        t = 0
        for i in range(SYMBOLS):
            f[i] = (f[i] + 1) >> 1
            t += f[i]
        m.total = t


class _Encoder:
    def __init__(self) -> None:
        self.low = 0
        self.range = 0xFFFFFFFF
        self.cache = 0
        self.pending = 1
        self.out = bytearray()

    def _shift(self) -> None:
        if self.low < 0xFF000000 or self.low >= 1 << 32:
            carry = self.low >> 32
            byte = self.cache
            while True:
                self.out.append((byte + carry) & 0xFF)
                byte = 0xFF
                self.pending -= 1
                if not self.pending:
                    break
            self.cache = (self.low >> 24) & 0xFF
        self.pending += 1
        self.low = (self.low << 8) & 0xFFFFFFFF

    def encode(self, m: _Model, s: int) -> None:
        f = m.freq
        cum = 0
        for i in range(s):
            cum += f[i]
        r = self.range // m.total
        self.low += cum * r
        self.range = f[s] * r
        while self.range < TOP:
            self.range <<= 8
            self._shift()
        _update(m, s)

    def finish(self) -> bytes:
        for _ in range(5):
            self._shift()
        return bytes(self.out[1:])  # the first byte out is the empty cache, always 0


class _Decoder:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0
        self.range = 0xFFFFFFFF
        self.code = 0
        for _ in range(4):
            self.code = (self.code << 8) | self._byte()

    def _byte(self) -> int:
        b = self.data[self.pos] if self.pos < len(self.data) else 0
        self.pos += 1
        return b

    def decode(self, m: _Model) -> int:
        f = m.freq
        r = self.range // m.total
        v = min(self.code // r, m.total - 1)
        s = cum = 0
        while cum + f[s] <= v:
            cum += f[s]
            s += 1
        self.code -= cum * r
        self.range = f[s] * r
        while self.range < TOP:
            self.range <<= 8
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
        _update(m, s)
        return s

    def exact(self) -> bool:
        return self.pos == len(self.data)


# --- luma --------------------------------------------------------------------


def _slope(d: int) -> int:
    """The row above's slope at a pixel, up-right less up-left, in five bands."""
    return 0 if d <= -3 else 1 if d < 0 else 2 if d == 0 else 3 if d < 3 else 4


def luma_contexts(codes: np.ndarray) -> np.ndarray:
    """Each pixel's context, from its neighbours as the decoder has them:
    left, up, up-left and up-right, OUTSIDE past the edges."""
    h, w = codes.shape
    p = np.full((h + 1, w + 2), OUTSIDE, np.int64)
    p[1:, 1:-1] = codes
    left, up, ul, ur = p[1:, :-2], p[:-1, 1:-1], p[:-1, :-2], p[:-1, 2:]
    d = ur - ul
    slope = np.select([d <= -3, d < 0, d == 0, d < 3], [0, 1, 2, 3], 4)
    return (up * 16 + left) * 5 + slope


_prior: list[list[int]] | None = None


def luma_prior() -> list[list[int]]:
    """The learned starting counts, read from the header the firmware compiles."""
    global _prior
    if _prior is None:
        text = PRIOR_HEADER.read_text()
        body = text[text.index("= {") :]
        values = [int(v) for v in re.findall(r"\d+", body)]
        if len(values) != LUMA_CONTEXTS * SYMBOLS:
            raise SystemExit(f"{PRIOR_HEADER}: {len(values)} counts, not {LUMA_CONTEXTS * SYMBOLS}")
        _prior = [values[i : i + SYMBOLS] for i in range(0, len(values), SYMBOLS)]
    return _prior


def encode_luma(codes: np.ndarray) -> bytes:
    models = [_Model(list(row)) for row in luma_prior()]
    enc = _Encoder()
    for c, s in zip(luma_contexts(codes).ravel().tolist(), codes.ravel().tolist(), strict=True):
        enc.encode(models[c], s)
    return enc.finish()


def decode_luma(data: bytes, w: int, h: int) -> np.ndarray:
    """The plane back, each pixel's context from pixels already decoded, as
    the device does. Raises on a stream that is not exactly one plane."""
    models = [_Model(list(row)) for row in luma_prior()]
    dec = _Decoder(data)
    rows = [[OUTSIDE] * (w + 2) for _ in range(h + 1)]
    for y in range(h):
        above, row = rows[y], rows[y + 1]
        for x in range(w):
            c = (above[x + 1] * 16 + row[x]) * 5 + _slope(above[x + 2] - above[x])
            row[x + 1] = dec.decode(models[c])
    if not dec.exact():
        raise ValueError("luma stream is not one plane")
    return np.array(rows, np.uint8)[1:, 1:-1]


# --- chroma ------------------------------------------------------------------


def block_luma(luma: np.ndarray, block: int) -> tuple[np.ndarray, np.ndarray]:
    """Which blocks have anything painted in them, and each painted block's
    luma band (0-3): the mean of its painted codes, integer, over 15 in four."""
    h, w = luma.shape
    bh, bw = (h + block - 1) // block, (w + block - 1) // block
    p = np.full((bh * block, bw * block), OUTSIDE, np.int64)
    p[:h, :w] = luma
    cells = p.reshape(bh, block, bw, block).transpose(0, 2, 1, 3).reshape(bh, bw, -1)
    inside = cells != OUTSIDE
    n = inside.sum(axis=2)
    total = np.where(inside, cells, 0).sum(axis=2)
    mean = total // np.maximum(n, 1)
    return n > 0, np.minimum(3, mean * 4 // 15)


def encode_chroma(chroma: np.ndarray, luma: np.ndarray, block: int) -> bytes:
    painted, band = block_luma(luma, block)
    bh, bw = painted.shape
    known = np.where(painted, chroma, CHROMA_EDGE).astype(np.int64)
    p = np.full((bh + 1, bw + 1), CHROMA_EDGE, np.int64)
    p[1:, 1:] = known
    ctx = ((p[:-1, 1:] * 17 + p[1:, :-1]) * 4 + band)[painted].tolist()
    models = [_Model([1] * SYMBOLS) for _ in range(CHROMA_CONTEXTS)]
    enc = _Encoder()
    for c, s in zip(ctx, chroma[painted].tolist(), strict=True):
        enc.encode(models[c], s)
    return enc.finish()


def decode_chroma(data: bytes, luma: np.ndarray, block: int) -> np.ndarray:
    """The chroma codes back; an unpainted block comes back 0."""
    painted, band = block_luma(luma, block)
    bh, bw = painted.shape
    models = [_Model([1] * SYMBOLS) for _ in range(CHROMA_CONTEXTS)]
    dec = _Decoder(data)
    out = np.zeros((bh, bw), np.uint8)
    rows = [[CHROMA_EDGE] * (bw + 1) for _ in range(bh + 1)]
    for y in range(bh):
        for x in range(bw):
            if not painted[y, x]:
                continue
            s = dec.decode(models[(rows[y][x + 1] * 17 + rows[y + 1][x]) * 4 + int(band[y, x])])
            rows[y + 1][x + 1] = s
            out[y, x] = s
    if not dec.exact():
        raise ValueError("chroma stream is not one plane")
    return out


# --- the prior -----------------------------------------------------------------


def write_prior(counts: np.ndarray, note: str) -> None:
    """Counts a context -> rows summing to about PRIOR_STRENGTH, none below 1,
    written as the header both sides read."""
    p = counts + 0.5
    p = p / p.sum(axis=1, keepdims=True) * PRIOR_STRENGTH
    rows = np.maximum(1, np.rint(p)).astype(np.int64)
    lines = [
        "// Generated by firmware/tools/planecoder.py --train; do not edit.",
        f"// {note}",
        "// The luma model's starting counts, one row of 16 a context",
        "// ((up * 16 + left) * 5 + slope): see planecoder.py. The encoder and the",
        "// decoder must start from the same counts, so a change here needs every",
        "// pack rebaked.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace birdposter {",
        "",
        f"constexpr uint16_t kLumaPrior[{LUMA_CONTEXTS}][16] = {{",
    ]
    lines += ["    {" + ", ".join(str(v) for v in row) + "}," for row in rows.tolist()]
    lines += ["};", "", "}  // namespace birdposter", ""]
    PRIOR_HEADER.write_text("\n".join(lines))


def train(every: int, source: int) -> None:
    import sys

    import bake_plates as bp
    from bake_masks import silhouette, species_of

    counts = np.zeros((LUMA_CONTEXTS, SYMBOLS))
    plates = 0
    for style in ("au", "eu", "us"):
        chosen: dict[str, Path] = {}
        for path in sorted((bp.REPO / "assets" / "artwork" / style / "birds").glob("*.png")):
            chosen.setdefault(species_of(path), path)
        for name in sorted(chosen)[::every]:
            art = bp.scaled(chosen[name], source)
            mask, _ = silhouette(chosen[name], source)
            codes = bp.posterise(bp.composite(art), np.asarray(mask) > 0)["luma"]
            np.add.at(counts, (luma_contexts(codes).ravel(), codes.ravel()), 1)
            plates += 1
        print(f"  {style}: {plates} plates so far", file=sys.stderr)
    write_prior(counts, f"{plates} plates, every {every}th of au, eu and us, at {source} px.")
    print(f"{PRIOR_HEADER}: from {plates} plates")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--train", action="store_true", help="relearn the luma prior")
    ap.add_argument("--every", type=int, default=6, help="train on every Nth species")
    ap.add_argument("--source", type=int, default=448, help="the size to train at")
    args = ap.parse_args()
    if not args.train:
        ap.error("nothing to do: --train")
    train(args.every, args.source)


if __name__ == "__main__":
    main()
