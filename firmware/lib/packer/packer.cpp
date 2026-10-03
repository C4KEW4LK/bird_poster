#include "packer.h"

#include <algorithm>
#include <cmath>

namespace birdposter {
namespace {

constexpr int kBits = 64;

inline size_t wordsFor(int w) { return size_t((w + kBits - 1) / kBits) + 1; }  // +1 guard

// How many bits are set in a byte, and what their positions add up to. A column
// centroid wants both, and eight lookups per word is a great deal cheaper than
// sixty-four bit tests. 512 bytes of flash, resolved at compile time.
struct ByteStats {
  uint8_t count[256];
  uint8_t sum[256];
};

constexpr ByteStats makeByteStats() {
  ByteStats t{};
  for (int v = 0; v < 256; ++v) {
    uint8_t c = 0, s = 0;
    for (int b = 0; b < 8; ++b)
      if (v & (1 << b)) {
        ++c;
        s = uint8_t(s + b);
      }
    t.count[v] = c;
    t.sum[v] = s;
  }
  return t;
}

constexpr ByteStats kByteStats = makeByteStats();

// Any opaque pixel in columns [from, to) of one row. Both ends are masked off
// inside their own word, so a range costs a handful of word tests rather than
// a bit test per column.
bool anyInRange(const Mask &m, int y, int from, int to) {
  if (from >= to) return false;
  const uint64_t *r = m.row(y);
  const size_t first = size_t(from) >> 6, last = size_t(to - 1) >> 6;
  for (size_t i = first; i <= last; ++i) {
    uint64_t word = r[i];
    if (i == first) word &= ~0ULL << (from & 63);
    if (i == last) {
      const unsigned hi = unsigned((to - 1) & 63);
      if (hi < 63) word &= (1ULL << (hi + 1)) - 1;
    }
    if (word) return true;
  }
  return false;
}

// Set columns [from, to) of one row, the same way round.
void fillRange(Mask &m, int y, int from, int to) {
  if (from >= to) return;
  uint64_t *r = m.row(y);
  const size_t first = size_t(from) >> 6, last = size_t(to - 1) >> 6;
  for (size_t i = first; i <= last; ++i) {
    uint64_t word = ~0ULL;
    if (i == first) word &= ~0ULL << (from & 63);
    if (i == last) {
      const unsigned hi = unsigned((to - 1) & 63);
      if (hi < 63) word &= (1ULL << (hi + 1)) - 1;
    }
    r[i] |= word;
  }
}

}  // namespace

Mask::Mask(int w, int h) : w_(w), h_(h) {
  if (w <= 0 || h <= 0) {
    w_ = h_ = 0;
    return;
  }
  stride_ = wordsFor(w);
  bits_.assign(stride_ * size_t(h), 0);
}

bool Mask::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
  return (row(y)[x >> 6] >> (x & 63)) & 1ULL;
}

void Mask::set(int x, int y) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  row(y)[x >> 6] |= 1ULL << (x & 63);
}

int Mask::rowPopcount(int y) const {
  const uint64_t *r = row(y);
  int n = 0;
  for (size_t i = 0; i < stride_; ++i) {
#if defined(__GNUC__) || defined(__clang__)
    n += __builtin_popcountll(r[i]);
#else
    for (uint64_t v = r[i]; v; v &= v - 1) ++n;
#endif
  }
  return n;
}

Mask Mask::scaled(int dim, bool flip) const {
  if (empty() || dim <= 0) return Mask();
  const int longest = std::max(w_, h_);
  const double s = double(dim) / double(longest);
  const int nw = std::max(1, int(std::lround(w_ * s)));
  const int nh = std::max(1, int(std::lround(h_ * s)));
  Mask out(nw, nh);
  // Nearest neighbour, sampling the source pixel at the destination centre.
  // The Python resamples the alpha with LANCZOS and re-thresholds; on a
  // silhouette that is already binary the two differ only along the outline,
  // and the erosion below eats that difference.
  //
  // Every row samples the same source columns, so they are resolved once into a
  // table and the mirroring folded in. A divide per pixel was the whole cost of
  // this loop, and on a chip whose doubles are emulated in software it is not a
  // close call. The destination is then built a word at a time, so a row costs
  // one store per 64 columns instead of a read-modify-write per opaque pixel.
  std::vector<int> cols(size_t(nw), 0);
  for (int x = 0; x < nw; ++x) {
    const int sx = std::min(w_ - 1, int((x + 0.5) / s));
    cols[size_t(x)] = flip ? w_ - 1 - sx : sx;
  }
  for (int y = 0; y < nh; ++y) {
    const int sy = std::min(h_ - 1, int((y + 0.5) / s));
    const uint64_t *src = row(sy);
    uint64_t *dst = out.row(y);
    for (int x0 = 0; x0 < nw; x0 += kBits) {
      const int end = std::min(nw, x0 + kBits);
      uint64_t word = 0;
      for (int x = x0; x < end; ++x) {
        const int sx = cols[size_t(x)];
        if ((src[sx >> 6] >> (sx & 63)) & 1ULL) word |= 1ULL << (x - x0);
      }
      dst[size_t(x0) >> 6] = word;
    }
  }
  return out;
}

Mask Mask::eroded(int radius) const {
  if (empty() || radius <= 0) return *this;
  // Separable, and bit-parallel on both axes: a pixel survives only where the
  // whole (2r+1) square was set, which is what a minimum filter over a binary
  // image does. Horizontally that is the row ANDed with itself shifted each
  // way; vertically it is a word-wise AND of neighbouring rows, with no shift
  // at all. Sixty-four columns to the instruction rather than one - and this
  // was, per-pixel, the single most expensive thing the packer did.
  //
  // Off the edge still counts as empty, so the outline erodes inward - the same
  // as PIL, which pads the border rather than wrapping it. Nothing extra is
  // needed for that: bits past `width` are never set, the guard word is zero,
  // and a shift brings in zeros at either end.
  const size_t sw = stride_;
  Mask horiz(w_, h_);
  for (int y = 0; y < h_; ++y) {
    const uint64_t *src = row(y);
    uint64_t *dst = horiz.row(y);
    for (size_t i = 0; i < sw; ++i) dst[i] = src[i];
    for (int d = 1; d <= radius; ++d) {
      const size_t off = size_t(d) >> 6;
      const unsigned s = unsigned(d) & 63;
      for (size_t i = 0; i < sw; ++i) {
        uint64_t lower = 0;  // the pixel d columns to the left
        if (i >= off) {
          lower = src[i - off] << s;
          if (s && i > off) lower |= src[i - off - 1] >> (kBits - s);
        }
        uint64_t upper = 0;  // and d to the right
        if (i + off < sw) {
          upper = src[i + off] >> s;
          if (s && i + off + 1 < sw) upper |= src[i + off + 1] << (kBits - s);
        }
        dst[i] &= lower & upper;
      }
    }
  }
  Mask out(w_, h_);
  // Rows within `radius` of either edge have a neighbour off the mask, so they
  // erode away entirely and are left as the zeros they were allocated with.
  for (int y = radius; y + radius < h_; ++y) {
    const uint64_t *base = horiz.row(y);
    uint64_t *dst = out.row(y);
    for (size_t i = 0; i < sw; ++i) dst[i] = base[i];
    for (int d = 1; d <= radius; ++d) {
      const uint64_t *lo = horiz.row(y - d);
      const uint64_t *hi = horiz.row(y + d);
      for (size_t i = 0; i < sw; ++i) dst[i] &= lo[i] & hi[i];
    }
  }
  return out;
}

bool collides(const Mask &grid, const Mask &sprite, int x, int y) {
  const size_t sw = sprite.stride() - 1;  // real words; the last is the guard
  for (int r = 0; r < sprite.height(); ++r) {
    const uint64_t *srow = sprite.row(r);
    const uint64_t *grow = grid.row(y + r);
    for (size_t i = 0; i < sw; ++i) {
      const uint64_t s = srow[i];
      if (!s) continue;
      const size_t bit = size_t(x) + i * kBits;
      const size_t w0 = bit >> 6;
      const unsigned sh = unsigned(bit & 63);
      uint64_t g = grow[w0] >> sh;
      if (sh) g |= grow[w0 + 1] << (kBits - sh);  // the guard word makes this safe
      if (g & s) return true;
    }
  }
  return false;
}

void stamp(Mask &grid, const Mask &sprite, int x, int y) {
  const size_t sw = sprite.stride() - 1;
  for (int r = 0; r < sprite.height(); ++r) {
    const uint64_t *srow = sprite.row(r);
    uint64_t *grow = grid.row(y + r);
    for (size_t i = 0; i < sw; ++i) {
      const uint64_t s = srow[i];
      if (!s) continue;
      const size_t bit = size_t(x) + i * kBits;
      const size_t w0 = bit >> 6;
      const unsigned sh = unsigned(bit & 63);
      grow[w0] |= s << sh;
      if (sh) grow[w0 + 1] |= s >> (kBits - sh);
    }
  }
}

namespace {

// Opaque pixels per row, wanted twice over: to pick the probe rows and to order
// the full scan. Counted once and shared.
void rowCounts(const Mask &m, std::vector<int> &out) {
  out.assign(size_t(std::max(0, m.height())), 0);
  for (int y = 0; y < m.height(); ++y) out[size_t(y)] = m.rowPopcount(y);
}

// One row per horizontal band of a sprite, tested before its whole footprint.
// Most candidate positions on a filling page collide and a row costs about a
// hundredth of the box. Banded rather than simply the densest rows, which would
// all land in the body and catch the same collisions as each other.
void probeRows(const std::vector<int> &counts, std::vector<int> &rows) {
  rows.clear();
  const int h = int(counts.size());
  if (h <= 0) return;
  for (int b = 0; b < kProbeBands; ++b) {
    const int lo = (h * b) / kProbeBands;
    const int hi = (h * (b + 1)) / kProbeBands;
    int best = -1, bestCount = 0;
    for (int y = lo; y < hi; ++y) {
      const int c = counts[size_t(y)];
      if (c > bestCount) bestCount = c, best = y;
    }
    if (best >= 0) rows.push_back(best);
  }
}

// The rows the full footprint check walks, and in what order. Ordering cannot
// change the answer - the test is an OR over rows - but by the time a candidate
// reaches the full scan it has passed three probes and is still, measured,
// colliding 998 times in 1000, so the whole cost is how long it takes to reach
// the pixel that hits.
//
// Outermost first, alternating ends inward, because that is where the hit is:
// the spiral walks a bird outward until it stops overlapping, so a rejected
// candidate is one lying *against* a neighbour, and they meet at their edges.
// Scanning densest-first instead - the obvious guess, the body being where the
// pixels are - was measured 14x worse than this and slightly worse than doing
// nothing, because a mid-body row is exactly the part still in clear paper.
//
// Empty rows go because they can never contribute, and the probe rows because
// they are already known clear at this point.
void scanOrder(const std::vector<int> &counts, const std::vector<int> &probes,
               std::vector<int> &rows) {
  rows.clear();
  for (int y = 0; y < int(counts.size()); ++y) {
    if (counts[size_t(y)] <= 0) continue;
    if (std::find(probes.begin(), probes.end(), y) != probes.end()) continue;
    rows.push_back(y);
  }
  std::vector<int> ends;
  ends.reserve(rows.size());
  for (size_t lo = 0, hi = rows.size(); lo < hi;) {
    ends.push_back(rows[lo++]);
    if (lo < hi) ends.push_back(rows[--hi]);
  }
  rows.swap(ends);
}

bool rowCollides(const Mask &grid, const Mask &sprite, int r, int x, int y) {
  const size_t sw = sprite.stride() - 1;
  const uint64_t *srow = sprite.row(r);
  const uint64_t *grow = grid.row(y + r);
  for (size_t i = 0; i < sw; ++i) {
    const uint64_t s = srow[i];
    if (!s) continue;
    const size_t bit = size_t(x) + i * kBits;
    const size_t w0 = bit >> 6;
    const unsigned sh = unsigned(bit & 63);
    uint64_t g = grow[w0] >> sh;
    if (sh) g |= grow[w0 + 1] << (kBits - sh);
    if (g & s) return true;
  }
  return false;
}

// `collides`, over a given set of rows. Same answer as the whole footprint
// whenever the rows left out are empty or already known clear.
bool collidesOrdered(const Mask &grid, const Mask &sprite, const std::vector<int> &rows, int x,
                     int y) {
  for (int r : rows)
    if (rowCollides(grid, sprite, r, x, y)) return true;
  return false;
}

}  // namespace

Sprite withLabel(int index, int dim, const Mask &art, const LabelBox &label, int gap) {
  Sprite s;
  s.index = index;
  s.dim = dim;
  if (label.w <= 0 || label.h <= 0) {
    s.mask = art;
    return s;
  }
  const int aw = art.width(), ah = art.height();

  // Column centroid of the silhouette: the name belongs under the body, not
  // half way out along a tail. Summed a byte at a time off the table above.
  // 64-bit because `long` is 32 bits on the xtensa and the sum is a pixel count
  // times a column index - the host and the board have to agree on the answer.
  long long sum = 0, count = 0;
  for (int y = 0; y < ah; ++y) {
    const uint64_t *r = art.row(y);
    for (size_t i = 0; i < art.stride(); ++i) {
      uint64_t word = r[i];
      if (!word) continue;
      const int base = int(i) * kBits;
      for (int b = 0; b < 8 && word; ++b, word >>= 8) {
        const uint8_t v = uint8_t(word & 0xff);
        if (!v) continue;
        count += kByteStats.count[v];
        sum += (long long)(kByteStats.count[v]) * (base + b * 8) + kByteStats.sum[v];
      }
    }
  }
  const double centre = count ? double(sum) / double(count) : aw / 2.0;

  // Both bounds off one rounded edge; rounding them apart clips the box a
  // column short.
  const int offset = int(std::lround(centre - label.w / 2.0));
  const int left = std::min(0, offset);
  const int width = std::max(aw, offset + label.w) - left;
  const int ax = -left, lx = offset - left;

  // Raise the name until it clears the outline directly above it, so it sits in
  // the gap beside a leg rather than below the whole bounding box.
  const int from = std::max(0, lx - ax);
  const int to = std::min(aw, lx - ax + label.w);
  int bottom = -1;
  for (int y = ah - 1; y >= 0; --y)
    if (anyInRange(art, y, from, to)) {
      bottom = y;
      break;
    }
  const int top = (bottom >= 0 ? bottom + 1 : ah) + gap;
  const int height = std::max(ah, top + label.h);

  s.mask = Mask(width, height);
  // The art moves in whole rows, which is what `stamp` already does.
  stamp(s.mask, art, ax, 0);
  // The reserved box is solid in the collision mask - that is the whole point.
  for (int y = top; y < top + label.h; ++y) fillRange(s.mask, y, lx, lx + label.w);

  s.artX = ax;
  s.artY = 0;
  s.labelX = lx;
  s.labelY = top;
  s.labelW = label.w;
  s.labelH = label.h;
  s.hasLabel = true;
  return s;
}

namespace {

// The spiral's search for one sprite: the first candidate on an elliptical
// spiral out from the canvas centre that does not collide. This is `pack`'s
// inner loop, lifted out so the coarse packer can fall back to it.
bool spiralPlace(const Mask &occ, const Sprite &sprite, int width, int height, float turn,
                 std::vector<int> &counts, std::vector<int> &probes, std::vector<int> &order,
                 int &fx, int &fy) {
  const double maxR = std::hypot(double(width), double(height));
  const double cx = width / 2.0, cy = height / 2.0;
  const int w = sprite.mask.width(), h = sprite.mask.height();
  rowCounts(sprite.mask, counts);
  probeRows(counts, probes);
  scanOrder(counts, probes, order);

  // Walk a spiral of candidates out from the canvas centre, so the birds
  // sorted largest-first land in the middle. First non-colliding wins.
  //
  // The sweep is an ellipse matched to the page, not a circle. A circular
  // sweep grows a circular cluster, and a disc inscribed in a 4:3 page reaches
  // 59% of it - the corners are not merely empty, they are unreachable. The
  // ellipse costs nothing and makes them candidates like anywhere else.
  const double ax = kEllipticalSpiral ? double(width) / std::min(width, height) : 1.0;
  const double ay = kEllipticalSpiral ? double(height) / std::min(width, height) : 1.0;
  // How far the candidate's centre may stray from the page's before the
  // sprite hangs off an edge. The +1 is the truncation in `int(px - w/2.0)`,
  // which still yields 0 for a value just above -1.
  const double limX = (width - w) / 2.0 + 1.0, limY = (height - h) / 2.0 + 1.0;
  for (double r = 0.0; r <= maxR; r += kStep) {
    // Once the sweep's ellipse has cleared that box on both axes at once,
    // no angle is left that lands on the page and the walk is finished. The
    // radius ran to the page's diagonal before, which on a sprite too big to
    // place meant a couple of hundred laps entirely off the paper - and a
    // set that starts too big for the page is exactly what the scale search
    // opens with, so those laps were being walked several times a layout.
    if (r > 0.0) {
      if (limX <= 0.0 || limY <= 0.0) break;
      const double u = limX / (r * ax), v = limY / (r * ay);
      if (u <= 1.0 && v <= 1.0 && u * u + v * v <= 1.0) break;
    }
    const int count =
        (r == 0.0) ? 1 : std::max(8, int(2 * M_PI * r * std::max(ax, ay) / kStep));
    for (int i = 0; i < count; ++i) {
      const double a = double(turn) + 2 * M_PI * i / count;
      // One axis at a time: most of the sweep leaves the page on x alone, and
      // a candidate rejected there should not pay for a second transcendental.
      const double px = (r == 0.0) ? cx : cx + r * ax * std::cos(a);
      const int x = int(px - w / 2.0);
      if (x < 0 || x + w > width) continue;
      const double py = (r == 0.0) ? cy : cy + r * ay * std::sin(a);
      const int y = int(py - h / 2.0);
      if (y < 0 || y + h > height) continue;

      bool hit = false;
      for (int pr : probes) {
        if (rowCollides(occ, sprite.mask, pr, x, y)) {
          hit = true;
          break;
        }
      }
      if (hit) continue;  // a colliding probe row is a real collision
      if (collidesOrdered(occ, sprite.mask, order, x, y)) continue;
      fx = x, fy = y;
      return true;
    }
  }
  return false;
}

}  // namespace

bool pack(const std::vector<Sprite> &sprites, int width, int height,
          std::vector<Placement> &out, float turn) {
  Mask occ(width, height);
  out.clear();
  out.reserve(sprites.size());
  std::vector<int> counts, probes, order;
  for (const Sprite &sprite : sprites) {
    int fx = 0, fy = 0;
    if (!spiralPlace(occ, sprite, width, height, turn, counts, probes, order, fx, fy))
      return false;
    stamp(occ, sprite.mask, fx, fy);
    out.push_back({sprite.index, sprite.dim, fx + sprite.artX, fy + sprite.artY,
                   fx + sprite.labelX, fy + sprite.labelY, sprite.labelW, sprite.labelH,
                   sprite.hasLabel, sprite.labelPx});
  }
  return true;
}

// ---------------------------------------------------------------------------
// PROTOTYPE: coarse-to-fine contact packer.
//
// Two occupancy grids per level, both padded by `pad` cells so a contact ring
// hanging over the page edge is still in bounds: `n` has the outside clear and
// `b` has it set. Collision is tested against `b` (which also keeps the sprite
// on the page); contact is counted against both, and the difference is what the
// sprite touches of the page edge, weighted separately from a neighbour.

namespace {

// Set bits of `sprite` at (x, y) that are also set in `grid`, counted.
int popOverlap(const Mask &grid, const Mask &sprite, int x, int y) {
  const size_t sw = sprite.stride() - 1;
  int n = 0;
  for (int r = 0; r < sprite.height(); ++r) {
    const uint64_t *srow = sprite.row(r);
    const uint64_t *grow = grid.row(y + r);
    for (size_t i = 0; i < sw; ++i) {
      const uint64_t s = srow[i];
      if (!s) continue;
      const size_t bit = size_t(x) + i * kBits;
      const size_t w0 = bit >> 6;
      const unsigned sh = unsigned(bit & 63);
      uint64_t g = grow[w0] >> sh;
      if (sh) g |= grow[w0 + 1] << (kBits - sh);
#if defined(__GNUC__) || defined(__clang__)
      n += __builtin_popcountll(g & s);
#else
      for (uint64_t v = g & s; v; v &= v - 1) ++n;
#endif
    }
  }
  return n;
}

// The cells within `radius` of `m` (square neighbourhood), minus `m` itself:
// where a neighbour would have to be for the two to touch. Padded by `radius`
// on every side, so place it at (x - radius, y - radius).
Mask contactRing(const Mask &m, int radius) {
  const int w = m.width() + 2 * radius, h = m.height() + 2 * radius;
  Mask padded(w, h);
  stamp(padded, m, radius, radius);
  // Separable OR-dilation, the mirror of `eroded`.
  Mask horiz(w, h);
  const size_t sw = padded.stride();
  for (int y = 0; y < h; ++y) {
    const uint64_t *src = padded.row(y);
    uint64_t *dst = horiz.row(y);
    for (size_t i = 0; i < sw; ++i) dst[i] = src[i];
    for (int d = 1; d <= radius; ++d) {
      const size_t off = size_t(d) >> 6;
      const unsigned s = unsigned(d) & 63;
      for (size_t i = 0; i < sw; ++i) {
        uint64_t lower = 0;
        if (i >= off) {
          lower = src[i - off] << s;
          if (s && i > off) lower |= src[i - off - 1] >> (kBits - s);
        }
        uint64_t upper = 0;
        if (i + off < sw) {
          upper = src[i + off] >> s;
          if (s && i + off + 1 < sw) upper |= src[i + off + 1] << (kBits - s);
        }
        dst[i] |= lower | upper;
      }
    }
  }
  Mask ring(w, h);
  for (int y = 0; y < h; ++y) {
    uint64_t *dst = ring.row(y);
    const uint64_t *self = padded.row(y);
    for (int d = -radius; d <= radius; ++d) {
      const int yy = y + d;
      if (yy < 0 || yy >= h) continue;
      const uint64_t *src = horiz.row(yy);
      for (size_t i = 0; i < sw; ++i) dst[i] |= src[i];
    }
    for (size_t i = 0; i < sw; ++i) dst[i] &= ~self[i];
  }
  return ring;
}

// OR-downsample by kCoarse: a cell is set if any pixel in its block is.
// Conservative, so a clear test on the coarse grid is a clear test on the
// fine one for a placement aligned to the block. `pad` cells of border are
// added on every side and, in `b`, set.
void coarsen(const Mask &fine, int fineW, int fineH, int pad, Mask &n, Mask &b) {
  const int cw = (fineW + kCoarse - 1) / kCoarse, ch = (fineH + kCoarse - 1) / kCoarse;
  n = Mask(cw + 2 * pad, ch + 2 * pad);
  b = Mask(cw + 2 * pad, ch + 2 * pad);
  for (int y = 0; y < fineH; ++y) {
    const uint64_t *src = fine.row(y);
    uint64_t *dst = n.row(y / kCoarse + pad);
    for (size_t i = 0; i < fine.stride(); ++i) {
      uint64_t word = src[i];
      if (!word) continue;
      for (int bb = 0; bb < 8 && word; ++bb, word >>= 8) {
        if (!(word & 0xff)) continue;
        const int cx = int(i) * 8 + bb + pad;  // kCoarse == 8: one byte per cell
        dst[cx >> 6] |= 1ULL << (cx & 63);
      }
    }
  }
  for (int y = 0; y < b.height(); ++y) {
    uint64_t *dst = b.row(y);
    const uint64_t *src = n.row(y);
    const bool edge = y < pad || y >= b.height() - pad;
    for (size_t i = 0; i < b.stride(); ++i) dst[i] = src[i];
    if (edge) fillRange(b, y, 0, b.width());
    else {
      fillRange(b, y, 0, pad);
      fillRange(b, y, b.width() - pad, b.width());
    }
  }
}

// Chebyshev distance from every cell to the nearest obstacle, where an
// obstacle is a cell whose bit equals `obstacle`. Cells off the mask count as
// obstacles too. Two chamfer passes.
std::vector<int> chebyshevDT(const Mask &m, bool obstacle) {
  const int w = m.width(), h = m.height();
  const int inf = w + h + 2;
  std::vector<int> d(size_t(w) * h, inf);
  auto at = [&](int x, int y) -> int & { return d[size_t(y) * w + x]; };
  auto edge = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (m.get(x, y) == obstacle) { at(x, y) = 0; continue; }
      int best = at(x, y);
      const int nb[4][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}};
      for (const auto &n : nb) {
        const int nx = x + n[0], ny = y + n[1];
        best = std::min(best, (edge(nx, ny) ? 0 : at(nx, ny)) + 1);
      }
      at(x, y) = best;
    }
  for (int y = h - 1; y >= 0; --y)
    for (int x = w - 1; x >= 0; --x) {
      if (at(x, y) == 0) continue;
      int best = at(x, y);
      const int nb[4][2] = {{1, 1}, {0, 1}, {-1, 1}, {1, 0}};
      for (const auto &n : nb) {
        const int nx = x + n[0], ny = y + n[1];
        best = std::min(best, (edge(nx, ny) ? 0 : at(nx, ny)) + 1);
      }
      at(x, y) = best;
    }
  return d;
}

struct Pocket {
  int x, y, r;  // centre cell and Chebyshev radius, in the padded coarse grid
};

// The free pockets of the coarse grid: local maxima of the distance transform,
// largest first, with anything inside an already-kept pocket dropped.
std::vector<Pocket> pockets(const Mask &occ, int minR) {
  const int w = occ.width(), h = occ.height();
  const std::vector<int> d = chebyshevDT(occ, true);
  std::vector<Pocket> all;
  for (int y = 1; y + 1 < h; ++y)
    for (int x = 1; x + 1 < w; ++x) {
      const int v = d[size_t(y) * w + x];
      if (v < std::max(1, minR)) continue;
      bool top = true;
      for (int dy = -1; dy <= 1 && top; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          if (d[size_t(y + dy) * w + x + dx] > v) { top = false; break; }
      if (top) all.push_back({x, y, v});
    }
  std::sort(all.begin(), all.end(), [](const Pocket &a, const Pocket &b) { return a.r > b.r; });
  std::vector<Pocket> kept;
  for (const Pocket &p : all) {
    bool inside = false;
    for (const Pocket &k : kept)
      if (std::max(std::abs(p.x - k.x), std::abs(p.y - k.y)) <= k.r) { inside = true; break; }
    if (!inside) kept.push_back(p);
    if (kept.size() >= 64) break;
  }
  return kept;
}

Mask coarsenSprite(const Mask &m) {
  Mask n, b;
  coarsen(m, m.width(), m.height(), 0, n, b);
  return n;
}

}  // namespace

bool packGrid(const std::vector<Sprite> &sprites, int width, int height,
              std::vector<Placement> &out, const PackOptions &opt) {
  const int n = int(sprites.size());
  out.clear();
  out.reserve(size_t(n));
  if (n == 0) return false;

  // Columns matched to the page's aspect, so the cells come out roughly square
  // whichever way round the page is: cols/rows ~ width/height with cols*rows ~ n.
  int cols = std::max(1, int(std::lround(std::sqrt(double(n) * width / height))));
  cols = std::min(cols, n);
  const int rows = (n + cols - 1) / cols;
  // Spread the remainder over the rows rather than leaving a short last row,
  // which would sit a lonely bird in a corner of an otherwise even page.
  std::vector<int> perRow(size_t(rows), n / rows);
  for (int r = 0; r < n % rows; ++r) perRow[size_t(r)] += 1;

  Mask occ(width, height);
  int i = 0;
  for (int r = 0; r < rows; ++r) {
    const int cnt = perRow[size_t(r)];
    if (cnt <= 0) continue;
    const double cellW = double(width) / cnt, cellH = double(height) / rows;
    for (int j = 0; j < cnt; ++j, ++i) {
      const Sprite &sprite = sprites[size_t(i)];
      const Mask &m = sprite.mask;
      // Half a cell across on alternate rows: the same trick that makes a
      // hexagonal packing denser than a square one, since a bird then faces
      // the gap between the two above it rather than one of them squarely.
      const double shift = (opt.brick && (r & 1)) ? cellW / 2.0 : 0.0;
      const double cx = (j + 0.5) * cellW + shift, cy = (r + 0.5) * cellH;
      int x = int(std::lround(cx - m.width() / 2.0));
      int y = int(std::lround(cy - m.height() / 2.0));
      // A cell at the edge cannot hold a bird wider than itself; clamping is
      // what keeps the lattice on the page instead of failing the scale.
      x = std::max(0, std::min(x, width - m.width()));
      y = std::max(0, std::min(y, height - m.height()));
      if (m.width() > width || m.height() > height) return false;
      if (collides(occ, m, x, y)) return false;  // too big for this spacing: shrink
      stamp(occ, m, x, y);
      out.push_back({sprite.index, sprite.dim, x + sprite.artX, y + sprite.artY,
                     x + sprite.labelX, y + sprite.labelY, sprite.labelW, sprite.labelH,
                     sprite.hasLabel, sprite.labelPx});
    }
  }
  return true;
}

namespace {

// xorshift32: the jitter has to be the same on the host and the board, so the
// generator is written out rather than taken from the library.
inline uint32_t nextRand(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

}  // namespace

bool packHero(const std::vector<Sprite> &sprites, int width, int height,
              std::vector<Placement> &out, const PackOptions &opt) {
  const int n = int(sprites.size());
  out.clear();
  out.reserve(size_t(n));
  if (n == 0) return false;

  Mask occ(width, height);
  const double cx = width / 2.0, cy = height / 2.0;

  // The hero, dead centre. It was sized against the page, so this is where it
  // meets the edges.
  {
    const Mask &m = sprites[0].mask;
    if (m.width() > width || m.height() > height) return false;
    const int x = (width - m.width()) / 2, y = (height - m.height()) / 2;
    stamp(occ, m, x, y);
    out.push_back({sprites[0].index, sprites[0].dim, x + sprites[0].artX, y + sprites[0].artY,
                   x + sprites[0].labelX, y + sprites[0].labelY, sprites[0].labelW,
                   sprites[0].labelH, sprites[0].hasLabel, sprites[0].labelPx});
  }
  if (n == 1) return true;

  // One ray per remaining bird, evenly spaced around the circle, and each bird
  // walks out along its own until it clears the hero and its neighbours. The
  // even spacing is in angle, so a hero that spans the page simply pushes the
  // birds on the vertical rays further out than those on the horizontal ones,
  // and they end up beside it rather than above and below it.
  const double ax = double(width) / std::min(width, height);
  const double ay = double(height) / std::min(width, height);
  const double maxR = 0.5 * std::hypot(double(width), double(height));
  const int ring = n - 1;

  std::vector<int> counts, probes, order;
  for (int k = 1; k <= ring; ++k) {
    const Sprite &sprite = sprites[size_t(k)];
    const Mask &m = sprite.mask;
    if (m.width() > width || m.height() > height) return false;
    const int w = m.width(), h = m.height();

    bool found = false;
    int fx = 0, fy = 0;
    // The ray this bird owns, then a few rotations of it: a bird whose own
    // direction is blocked all the way out should lean on its neighbours'
    // spacing rather than fail the whole layout.
    for (int nudgeTurn = 0; nudgeTurn < 5 && !found; ++nudgeTurn) {
      const double lean = (nudgeTurn + 1) / 2 * (nudgeTurn & 1 ? 1.0 : -1.0) * (M_PI / ring);
      const double a = 2 * M_PI * double(k - 1) / ring + lean;
      for (double r = 0.0; r <= maxR && !found; r += kStep) {
        const int x = int(std::lround(cx + r * ax * std::cos(a) - w / 2.0));
        const int y = int(std::lround(cy + r * ay * std::sin(a) - h / 2.0));
        if (x < 0 || y < 0 || x + w > width || y + h > height) continue;
        if (collides(occ, m, x, y)) continue;
        fx = x, fy = y, found = true;
      }
    }
    // Nothing on any of its rays: let the spiral find it a place before the
    // whole scale is thrown away over one bird.
    if (!found) found = spiralPlace(occ, sprite, width, height, 0.0f, counts, probes, order, fx, fy);
    if (!found) return false;

    stamp(occ, m, fx, fy);
    out.push_back({sprite.index, sprite.dim, fx + sprite.artX, fy + sprite.artY,
                   fx + sprite.labelX, fy + sprite.labelY, sprite.labelW, sprite.labelH,
                   sprite.hasLabel, sprite.labelPx});
  }
  return true;
}

void voronoiSeeds(int n, int width, int height, const PackOptions &opt, std::vector<double> &seedX,
                  std::vector<double> &seedY, std::vector<double> &weight) {
  seedX.assign(size_t(std::max(0, n)), 0.0);
  seedY.assign(size_t(std::max(0, n)), 0.0);
  weight.assign(size_t(std::max(0, n)), 1.0);
  if (n <= 0) return;

  int cols = std::max(1, int(std::lround(std::sqrt(double(n) * width / height))));
  cols = std::min(cols, n);
  const int rows = (n + cols - 1) / cols;
  std::vector<int> perRow(size_t(rows), n / rows);
  for (int r = 0; r < n % rows; ++r) perRow[size_t(r)] += 1;

  // Seeds on the lattice, shaken by up to half a cell times `jitter`.
  uint32_t rng = 0x9e3779b9u ^ (opt.seed * 2654435761u) ^ uint32_t(n * 2246822519u);
  int i = 0;
  for (int r = 0; r < rows; ++r) {
    const int cnt = perRow[size_t(r)];
    const double cellW = double(width) / cnt, cellH = double(height) / rows;
    for (int j = 0; j < cnt; ++j, ++i) {
      const double jx = (double(nextRand(rng) & 0xffff) / 65535.0 - 0.5) * opt.jitter * cellW;
      const double jy = (double(nextRand(rng) & 0xffff) / 65535.0 - 0.5) * opt.jitter * cellH;
      seedX[size_t(i)] = (j + 0.5) * cellW + jx;
      seedY[size_t(i)] = (r + 0.5) * cellH + jy;
    }
  }

  // Lloyd relaxation over a coarse sampling of the page: each sample goes to
  // its nearest seed, then every seed moves to the mean of its samples. A few
  // passes and the seeds are evenly spread without being in rows. The last
  // pass also leaves the cell areas, which is one of the two ways a seed's
  // size is measured below.
  const int step = std::max(8, std::min(width, height) / 96);
  std::vector<int> cells(size_t(n), 0);
  for (int pass = 0; pass < std::max(1, opt.lloyd); ++pass) {
    std::vector<double> ax(size_t(n), 0.0), ay(size_t(n), 0.0);
    cells.assign(size_t(n), 0);
    for (int y = step / 2; y < height; y += step)
      for (int x = step / 2; x < width; x += step) {
        int best = 0;
        double bestD = 1e30;
        for (int k = 0; k < n; ++k) {
          const double dx = x - seedX[size_t(k)], dy = y - seedY[size_t(k)];
          const double d = dx * dx + dy * dy;
          if (d < bestD) bestD = d, best = k;
        }
        ax[size_t(best)] += x, ay[size_t(best)] += y, cells[size_t(best)] += 1;
      }
    // The areas of the last pass are wanted, but its move is not when the
    // caller asked for no relaxation at all.
    if (opt.lloyd <= 0) break;
    for (int k = 0; k < n; ++k)
      if (cells[size_t(k)]) {
        seedX[size_t(k)] = ax[size_t(k)] / cells[size_t(k)];
        seedY[size_t(k)] = ay[size_t(k)] / cells[size_t(k)];
      }
  }

  if (!opt.perSeed) return;

  // How much room a seed has, in pixels across: either the square root of its
  // Voronoi cell's area, or the distance to the nearest other seed. The two
  // differ at the page edge, where a cell is cut off by the paper but the
  // neighbour is not.
  std::vector<double> room(size_t(n), 0.0);
  for (int k = 0; k < n; ++k) {
    if (opt.cellSize) {
      room[size_t(k)] = std::sqrt(double(cells[size_t(k)]) * step * step);
    } else {
      double best = 1e30;
      for (int j = 0; j < n; ++j) {
        if (j == k) continue;
        const double dx = seedX[size_t(k)] - seedX[size_t(j)];
        const double dy = seedY[size_t(k)] - seedY[size_t(j)];
        best = std::min(best, std::hypot(dx, dy));
      }
      // The paper is a neighbour too: a seed against the edge has less room
      // than its distance to the next seed suggests, and sizing it off that
      // alone puts a bird half off the page for the scale search to undo.
      const double edge = 2.0 * std::min(std::min(seedX[size_t(k)], width - seedX[size_t(k)]),
                                         std::min(seedY[size_t(k)], height - seedY[size_t(k)]));
      room[size_t(k)] = std::min(best, std::max(1.0, edge));
    }
  }

  double mean = 0.0;
  for (double r : room) mean += r;
  mean /= double(n);
  if (mean <= 0.0) return;
  // Normalised to the average, and clamped: one lucky seed in a corner should
  // not be handed a bird four times everyone else's.
  const double lo = 1.0 / std::max(1.0f, opt.sizeSpread), hi = std::max(1.0f, opt.sizeSpread);
  for (int k = 0; k < n; ++k)
    weight[size_t(k)] = std::min(hi, std::max(lo, room[size_t(k)] / mean));
}

bool packVoronoi(const std::vector<Sprite> &sprites, int width, int height,
                 std::vector<Placement> &out, const PackOptions &opt) {
  const int n = int(sprites.size());
  out.clear();
  out.reserve(size_t(n));
  if (n == 0) return false;

  std::vector<double> seedX, seedY, weight;
  voronoiSeeds(n, width, height, opt, seedX, seedY, weight);

  Mask occ(width, height);
  for (int k = 0; k < n; ++k) {
    const Sprite &sprite = sprites[size_t(k)];
    const Mask &m = sprite.mask;
    if (m.width() > width || m.height() > height) return false;
    int x = int(std::lround(seedX[size_t(k)] - m.width() / 2.0));
    int y = int(std::lround(seedY[size_t(k)] - m.height() / 2.0));
    x = std::max(0, std::min(x, width - m.width()));
    y = std::max(0, std::min(y, height - m.height()));
    if (collides(occ, m, x, y)) return false;  // too big for this spacing: shrink
    stamp(occ, m, x, y);
    out.push_back({sprite.index, sprite.dim, x + sprite.artX, y + sprite.artY,
                   x + sprite.labelX, y + sprite.labelY, sprite.labelW, sprite.labelH,
                   sprite.hasLabel, sprite.labelPx});
  }
  return true;
}

bool packCoarse(const std::vector<Sprite> &sprites, int width, int height,
                std::vector<Placement> &out, const PackOptions &opt) {
  static_assert(kCoarse == 8, "coarsen() reads one byte per cell");
  out.clear();
  out.reserve(sprites.size());

  // Full-resolution occupancy, padded so the fine contact ring stays in bounds.
  const int fpad = kFineRing + kFineWindow;
  Mask occ(width, height);  // unpadded: what `coarsen` reads and the fallback packs on
  Mask fineN(width + 2 * fpad, height + 2 * fpad), fineB(width + 2 * fpad, height + 2 * fpad);
  for (int y = 0; y < fineB.height(); ++y) {
    if (y < fpad || y >= fpad + height) fillRange(fineB, y, 0, fineB.width());
    else {
      fillRange(fineB, y, 0, fpad);
      fillRange(fineB, y, fpad + width, fineB.width());
    }
  }

  const int cpad = kCoarseRing;
  Mask coarseN, coarseB;
  coarsen(occ, width, height, cpad, coarseN, coarseB);

  const double pcx = width / 2.0, pcy = height / 2.0;
  const double ax = double(width) / std::min(width, height);
  const double ay = double(height) / std::min(width, height);
  const double maxD = std::hypot(pcx / ax, pcy / ay);

  std::vector<int> counts, probes, order;
  for (size_t k = 0; k < sprites.size(); ++k) {
    const Sprite &sprite = sprites[k];
    const Mask &m = sprite.mask;
    const int w = m.width(), h = m.height();
    if (w > width || h > height) return false;

    int fx = 0, fy = 0;
    bool found = false;
    if (k == 0) {
      // The first bird takes the middle of the page, as the spiral's does.
      fx = (width - w) / 2, fy = (height - h) / 2;
      found = true;
    } else {
      // Which coarse cells to score. `coarse` looks at every one; `pocket`
      // only at cells inside the free pockets a distance transform finds.
      const Mask cm = coarsenSprite(m);
      const Mask cring = contactRing(cm, kCoarseRing);
      const int cw = cm.width(), ch = cm.height();
      const int gridW = coarseN.width() - 2 * cpad, gridH = coarseN.height() - 2 * cpad;
      // Can the sprite sit at this cell at all - the cheap half of the score,
      // which is all a slide needs between one landing and the next.
      auto fitsCell = [&](int cx, int cy) -> bool {
        if (cx < 0 || cy < 0 || cx + cw > gridW || cy + ch > gridH) return false;
        if (cx * kCoarse + w > width || cy * kCoarse + h > height) return false;
        return !collides(coarseB, cm, cx + cpad, cy + cpad);
      };
      // A cell's contact score, or -inf when the sprite cannot sit there.
      auto scoreAt = [&](int cx, int cy) -> double {
        if (!fitsCell(cx, cy)) return -1e30;
        const int touchAll = popOverlap(coarseB, cring, cx + cpad - kCoarseRing, cy + cpad - kCoarseRing);
        const int touchN = popOverlap(coarseN, cring, cx + cpad - kCoarseRing, cy + cpad - kCoarseRing);
        const double sx = cx * kCoarse + w / 2.0 - pcx, sy = cy * kCoarse + h / 2.0 - pcy;
        const double dist = std::hypot(sx / ax, sy / ay) / maxD;  // 0 centre .. ~1 corner
        // A sub-contact jitter, deterministic in the seed: it cannot outweigh
        // a real difference in contact, only decide between equals, which is
        // what makes a second try a different page.
        double jitter = 0.0;
        if (opt.seed) {
          uint32_t hsh = opt.seed * 2654435761u;
          hsh ^= uint32_t(cx) * 2246822519u;
          hsh ^= uint32_t(cy) * 3266489917u;
          hsh ^= hsh >> 15;
          jitter = 0.49 * (double(hsh & 0xffff) / 65535.0);
        }
        return touchN + opt.borderWeight * (touchAll - touchN)
               - opt.centreWeight * dist * 2.0 * (cw + ch)  // scaled to the perimeter
               + jitter;
      };
      double best = -1e30;
      int bx = 0, by = 0;
      bool settle = true;
      if (opt.pocket) {
        // The bird's own inradius, in cells: a pocket narrower than its body
        // cannot hold it whatever its tail does.
        const std::vector<int> sd = chebyshevDT(cm, false);
        const int inR = *std::max_element(sd.begin(), sd.end());
        // Half the body's radius, not all of it: a pocket narrower than the
        // bird is still a seed worth sliding away from, and on a crowded page
        // the strict threshold leaves almost no seeds at all - which is where
        // this was losing to the full scan.
        const std::vector<Pocket> ps = pockets(coarseB, std::max(1, inR / 2));
        if (opt.airy) {
          // Middle of the biggest pocket the bird fits in, and nothing else.
          settle = false;
          for (const Pocket &p : ps) {
            const int cx = p.x - cpad - cw / 2, cy = p.y - cpad - ch / 2;
            if (scoreAt(cx, cy) > -1e30) { best = 0, bx = cx, by = cy; break; }
          }
        } else {
          // From each pocket's middle, slide the bird until it touches
          // something, once in each of eight directions. Scoring the landing
          // cells alone is what makes this cheap: a handful of candidates per
          // pocket instead of its whole area, and every one of them is against
          // a neighbour or an edge, which is where a dense pack wants the bird.
          const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                  {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
          // Slide until the next cell does not fit, collision only - the score
          // is wanted at the landing, not along the way.
          auto slide = [&](int cx, int cy, int dx, int dy, int &lx, int &ly) {
            for (int step = 0; step < gridW + gridH; ++step) {
              if (!fitsCell(cx + dx, cy + dy)) break;
              cx += dx, cy += dy;
            }
            lx = cx, ly = cy;
          };
          for (const Pocket &p : ps) {
            const int ox = p.x - cpad - cw / 2, oy = p.y - cpad - ch / 2;
            if (!fitsCell(ox, oy)) continue;  // the bird does not fit this pocket
            for (const auto &d : dirs) {
              int lx = 0, ly = 0;
              slide(ox, oy, d[0], d[1], lx, ly);
              const double sc = scoreAt(lx, ly);
              if (sc > best) best = sc, bx = lx, by = ly;
              // Then along the wall it landed against, both ways: resting on
              // one wall is a worse place than wedged into the corner where
              // two of them meet.
              const int perp[2][2] = {{d[1], d[0]}, {-d[1], -d[0]}};
              for (const auto &q : perp) {
                if (!q[0] && !q[1]) continue;
                int qx = 0, qy = 0;
                slide(lx, ly, q[0], q[1], qx, qy);
                const double qs = scoreAt(qx, qy);
                if (qs > best) best = qs, bx = qx, by = qy;
              }
            }
          }
        }
      }
      if (best <= -1e30) {
        // Every coarse position at once, scored by contact.
        settle = true;
        for (int cy = 0; cy + ch <= gridH; ++cy)
          for (int cx = 0; cx + cw <= gridW; ++cx) {
            const double sc = scoreAt(cx, cy);
            if (sc > best) best = sc, bx = cx, by = cy;
          }
      }
      if (best > -1e30 && !settle) {
        fx = bx * kCoarse, fy = by * kCoarse;
        found = !collides(fineB, m, fx + fpad, fy + fpad);
      } else if (best > -1e30) {
        // Settle at full resolution within the coarse cell: the offset with
        // the most contact that still fits, nearest the centre on a tie.
        const Mask fring = contactRing(m, kFineRing);
        const int x0 = bx * kCoarse, y0 = by * kCoarse;
        double fbest = -1e30;
        for (int dy = -kFineWindow; dy <= kFineWindow; ++dy) {
          for (int dx = -kFineWindow; dx <= kFineWindow; ++dx) {
            const int x = x0 + dx, y = y0 + dy;
            if (x < 0 || y < 0 || x + w > width || y + h > height) continue;
            if (collides(fineB, m, x + fpad, y + fpad)) continue;
            const int tAll = popOverlap(fineB, fring, x + fpad - kFineRing, y + fpad - kFineRing);
            const int tN = popOverlap(fineN, fring, x + fpad - kFineRing, y + fpad - kFineRing);
            const double sx = x + w / 2.0 - pcx, sy = y + h / 2.0 - pcy;
            const double dist = std::hypot(sx / ax, sy / ay) / maxD;
            const double score = tN + opt.borderWeight * (tAll - tN) - dist;
            if (score > fbest) fbest = score, fx = x, fy = y;
          }
        }
        found = fbest > -1e30;
      }
      // The coarse test is conservative: a gap that only fits at full
      // resolution is invisible to it. Let the spiral have a look before
      // giving up on the scale.
      if (!found)
        found = spiralPlace(occ, sprite, width, height, 0.0f, counts, probes, order, fx, fy);
    }
    if (!found) return false;

    stamp(occ, m, fx, fy);
    stamp(fineN, m, fx + fpad, fy + fpad);
    stamp(fineB, m, fx + fpad, fy + fpad);
    coarsen(occ, width, height, cpad, coarseN, coarseB);
    out.push_back({sprite.index, sprite.dim, fx + sprite.artX, fy + sprite.artY,
                   fx + sprite.labelX, fy + sprite.labelY, sprite.labelW, sprite.labelH,
                   sprite.hasLabel, sprite.labelPx});
  }
  return true;
}

namespace {
bool fitsAt(const Mask &occ, const Mask &m, int x, int y, int boxW, int boxH);
}

void compact(const std::vector<Sprite> &sprites, std::vector<Placement> &placed, int boxW,
             int boxH, int rounds, int step) {
  const size_t n = placed.size();
  if (n < 2 || step <= 0) return;
  for (int round = 0; round < rounds; ++round) {
    // Where the cluster is now, so a bird knows which way "inward" is.
    double sumX = 0, sumY = 0;
    for (size_t k = 0; k < n; ++k) {
      sumX += placed[k].x - sprites[k].artX + sprites[k].mask.width() / 2.0;
      sumY += placed[k].y - sprites[k].artY + sprites[k].mask.height() / 2.0;
    }
    const double cx = sumX / double(n), cy = sumY / double(n);

    bool moved = false;
    for (size_t k = 0; k < n; ++k) {
      Mask occ(boxW, boxH);
      for (size_t j = 0; j < n; ++j) {
        if (j == k) continue;
        stamp(occ, sprites[j].mask, placed[j].x - sprites[j].artX, placed[j].y - sprites[j].artY);
      }
      const Mask &m = sprites[k].mask;
      const int sx = placed[k].x - sprites[k].artX, sy = placed[k].y - sprites[k].artY;
      const double dx = cx - (sx + m.width() / 2.0), dy = cy - (sy + m.height() / 2.0);
      const double len = std::hypot(dx, dy);
      if (len < 1.0) continue;
      // Straight at the centre first, then the two axes alone: a bird blocked
      // on the diagonal can often still slide along one of them.
      const int ix = int(std::lround(step * dx / len)), iy = int(std::lround(step * dy / len));
      const int tries[3][2] = {{ix, iy}, {ix, 0}, {0, iy}};
      for (const auto &t : tries) {
        if (!t[0] && !t[1]) continue;
        const int nx = sx + t[0], ny = sy + t[1];
        if (!fitsAt(occ, m, nx, ny, boxW, boxH)) continue;
        placed[k].x += t[0], placed[k].y += t[1];
        placed[k].labelX += t[0], placed[k].labelY += t[1];
        moved = true;
        break;
      }
    }
    if (!moved) break;
  }
}

void center(std::vector<Placement> &placed, const std::vector<Sprite> &sprites,
            int width, int height) {
  if (placed.empty()) return;
  int x0 = width, y0 = height, x1 = 0, y1 = 0;
  for (size_t i = 0; i < placed.size(); ++i) {
    const Mask &m = sprites[i].mask;
    // The sprite's own origin, which the placement has already been offset from.
    const int sx = placed[i].x - sprites[i].artX, sy = placed[i].y - sprites[i].artY;
    x0 = std::min(x0, sx);
    y0 = std::min(y0, sy);
    x1 = std::max(x1, sx + m.width());
    y1 = std::max(y1, sy + m.height());
  }
  const int dx = (width - (x1 - x0)) / 2 - x0;
  const int dy = (height - (y1 - y0)) / 2 - y0;
  for (Placement &p : placed) {
    p.x += dx, p.y += dy;
    p.labelX += dx, p.labelY += dy;
  }
}


PackPlan planFor(PackStyle style) {
  PackPlan p;
  switch (style) {
    case PackStyle::Grid:
      p.pack.grid = true;
      p.pack.brick = true;  // offset rows: a bird grows into the gap between two, not against one
      break;
    case PackStyle::Scatter:
      p.pack.voronoi = true;
      p.pack.perSeed = true;  // sized by the room around its own seed
      break;
    case PackStyle::Hero:
      p.pack.hero = true;
      break;
    case PackStyle::Classic:
      break;  // the spiral, and the growth it has always had
  }
  if (style != PackStyle::Classic) {
    // Both of the placed styles leave the birds small and apart, so growth is
    // what actually packs the page: it needs room to move into, a ceiling well
    // above the 1.5x a spiral-packed bird can use, and a ration per round so
    // the first bird into a gap does not take all of it.
    p.growMax = 3.0f;
    p.growNudge = 24;
    p.growRounds = 20;
    p.growStep = 1.08f;
  }
  return p;
}

float turnFor(int layout) {
  const float turn = std::fmod(float(layout) * kGoldenAngle, 2.0f * float(M_PI));
  return turn < 0.0f ? turn + 2.0f * float(M_PI) : turn;
}

bool flipFor(bool baked, const char *name, int layout) {
  if (layout == 0 || name == nullptr) return baked;
  uint32_t h = 2166136261u;  // FNV-1a over "<name>/<layout>"
  for (const char *p = name; *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
  h = (h ^ uint8_t('/')) * 16777619u;
  for (uint32_t v = uint32_t(layout); ; v >>= 8) {
    h = (h ^ uint8_t(v & 0xff)) * 16777619u;
    if (v < 256) break;
  }
  return (h & 0xff) < 128;
}


namespace {

// A bird as the packer sees it at `dim`: the eroded silhouette, plus its
// name's box when names are on, scaled to `px`. The gap is taken off `px`
// rather than passed in, because `px` is per bird - the hero's name is set
// larger than the rest - and a gap measured against someone else's size would
// crowd the big name and cast the small ones adrift.
Sprite spriteAt(const std::vector<Mask> &sources, const std::vector<bool> &flips,
                const std::vector<LabelBox> &labels, int i, int dim, int px) {
  const Mask art = sources[i].scaled(dim, flips[i]).eroded(kOverlapPx);
  if (labels.empty()) {
    Sprite s;
    s.index = i;
    s.dim = dim;
    s.mask = art;
    return s;
  }
  // Baked at kLabelRefPx, so scale the box to the size this name landed at.
  const LabelBox box{int(std::lround(double(labels[i].w) * px / kLabelRefPx)),
                     int(std::lround(double(labels[i].h) * px / kLabelRefPx))};
  Sprite s = withLabel(i, dim, art, box, int(std::lround(px * kLabelGap)));
  s.labelPx = px;
  return s;
}

// `stamp` undone: the bird's own bits cleared from the grid. Exact because
// placed sprites never share a bit - every one was placed clear of the rest.
void unstamp(Mask &grid, const Mask &sprite, int x, int y) {
  const size_t sw = sprite.stride() - 1;
  for (int r = 0; r < sprite.height(); ++r) {
    const uint64_t *srow = sprite.row(r);
    uint64_t *g = grid.row(y + r);
    for (size_t i = 0; i < sw; ++i) {
      const uint64_t v = srow[i];
      if (!v) continue;
      const size_t bit = size_t(x) + i * kBits;
      const size_t w0 = bit >> 6;
      const unsigned sh = unsigned(bit & 63);
      g[w0] &= ~(v << sh);
      if (sh) g[w0 + 1] &= ~(v >> (kBits - sh));
    }
  }
}

bool fitsAt(const Mask &occ, const Mask &m, int x, int y, int boxW, int boxH) {
  if (x < 0 || y < 0 || x + m.width() > boxW || y + m.height() > boxH) return false;
  for (int r = 0; r < m.height(); ++r)
    if (rowCollides(occ, m, r, x, y)) return false;
  return true;
}

}  // namespace

bool layout(const std::vector<Mask> &sources, const std::vector<bool> &flips,
            const std::vector<LabelBox> &labels, int namePx, int pageW, int pageH, int boxW,
            int boxH, std::vector<Placement> &out, int *usedPx, int variant,
            const PackOptions &opt) {
  const size_t n = sources.size();
  if (n == 0) return false;

  // In the order given: the first bird takes the middle of the page.
  std::vector<int> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = int(i);
  if (opt.bigFirst)
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      const long A = long(sources[a].width()) * sources[a].height();
      const long B = long(sources[b].width()) * sources[b].height();
      return A > B;
    });

  // Overshoot deliberately: the set starts too big for the page and shrinks to
  // the first size that fits, which is what makes the collage fill the paper
  // rather than float in the middle of it. One size for every bird.
  // Off the page, not the box - see the note on the declaration.
  const double base =
      std::min(std::sqrt(double(pageW) * pageH * 1.5 / double(n)), std::min(pageW, pageH) * 0.7);

  // What each bird is worth in size, when the Voronoi packer is sizing by the
  // room around its own seed. Deterministic, and independent of the scale, so
  // it is worked out once and the scale search just multiplies through it.
  std::vector<double> weights(n, 1.0);
  if (opt.voronoi && opt.perSeed) {
    std::vector<double> sxs, sys;
    voronoiSeeds(int(n), boxW, boxH, opt, sxs, sys, weights);
  }
  // The bird in the middle is drawn larger from the start: growing into it
  // afterwards cannot work, because the ring is placed around whatever size
  // the hero was packed at.


  // One packing attempt at a given scale. Returns false if any bird did not fit.
  std::vector<Sprite> sprites;
  auto attemptAt = [&](double shrink, std::vector<Placement> &result, int &px) -> bool {
    sprites.clear();
    sprites.reserve(n);
    // Names have to shrink too: a fixed-size name never yields, so a full page
    // of them cannot converge at all.
    px = labels.empty() ? 0 : std::max(kMinLabelPx, int(std::lround(namePx * shrink)));
    // The hero's name is set larger, so it is not read as one more caption in
    // a page of them. It shrinks with the rest, being a multiple of their size.
    const bool heroOf = opt.hero && n > 1;
    const int heroPx = px == 0 ? 0 : std::max(px, int(std::lround(px * std::max(1.0f, opt.heroLabel))));
    const auto pxFor = [&](int i) { return heroOf && i == order[0] ? heroPx : px; };
    for (int i : order) {
      int dim = std::max(kMinDim, int(base * shrink * weights[size_t(i)]));
      // The hero is grown to the page's edges instead: the short side, so it
      // touches top and bottom of a landscape page or both sides of a portrait
      // one. Its sprite is the silhouette plus its name, and only the whole
      // sprite can be measured, so the size is solved for rather than derived
      // - two corrections land within a pixel or two.
      if (heroOf && i == order[0]) {
        // Grown until it touches the page - whichever pair of edges it reaches
        // first. Both axes have to be asked, not just the page's short side: a
        // tall bird on a portrait page meets top and bottom long before its
        // width runs out, and sizing it off the width alone makes a sprite
        // taller than the paper, which no scale then rescues because the
        // placer refuses the hero before the other birds are ever tried.
        const double fill = std::max(0.05, double(opt.heroFill));
        for (int it = 0; it < 5; ++it) {
          const Sprite trial = spriteAt(sources, flips, labels, i, dim, heroPx);
          const int tw = trial.mask.width(), th = trial.mask.height();
          if (tw <= 0 || th <= 0) break;
          const double room = std::min(boxW * fill / tw, boxH * fill / th);
          const int next = std::max(kMinDim, int(std::floor(dim * room)));
          const bool inside = tw <= boxW && th <= boxH;
          if (inside && next == dim) break;
          dim = next == dim ? dim - 1 : next;  // never stall on a sprite still over the edge
        }
      }
      sprites.push_back(spriteAt(sources, flips, labels, i, dim, pxFor(i)));
    }
    // Each try differs only in how ties are broken, so a scale fits if any of
    // them can place the set. The layout picks which run of tries: layout 0
    // is seeds 0.., the next the run after it, so every layout is a new page.
    bool ok = false;
    const int tries = std::max(1, opt.tries);
    for (int t = 0; t < tries && !ok; ++t) {
      PackOptions o = opt;
      o.seed = uint32_t(variant) * uint32_t(tries) + uint32_t(t);
      ok = o.hero    ? packHero(sprites, boxW, boxH, result, o)
         : o.voronoi ? packVoronoi(sprites, boxW, boxH, result, o)
         : o.grid   ? packGrid(sprites, boxW, boxH, result, o)
         : o.any()  ? packCoarse(sprites, boxW, boxH, result, o)
                    : pack(sprites, boxW, boxH, result, turnFor(variant + t));
    }
    if (!ok) return false;
    if (opt.compact) compact(sprites, result, boxW, boxH);
    center(result, sprites, boxW, boxH);
    return true;
  };

  int px = 0;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    const double shrink = std::pow(0.9, attempt);
    if (!attemptAt(shrink, out, px)) continue;

    // The geometric walk lands anywhere up to 11% under the largest size that
    // would have fitted, because it stops at the first success and the step is
    // 0.9. Close that gap by bisecting between this scale, which works, and the
    // one above it, which did not - or above the first attempt, which was never
    // tested against anything.
    double lo = shrink;                                  // fits
    double hi = shrink / 0.9;                            // does not (or untested, at attempt 0)
    std::vector<Placement> better;
    int betterPx = 0;
    for (int step = 0; step < std::max(0, opt.refineSteps); ++step) {
      const double mid = (lo + hi) / 2;
      if (attemptAt(mid, better, betterPx)) {
        lo = mid;
        out.swap(better);
        px = betterPx;
      } else {
        hi = mid;
      }
    }
    if (usedPx) *usedPx = px;
    return true;
  }
  return false;
}

void grow(const std::vector<Mask> &sources, const std::vector<bool> &flips,
          const std::vector<LabelBox> &labels, int namePx, int boxW, int boxH,
          std::vector<Placement> &placed, float maxFactor, int nudge, int rounds,
          float roundStep) {
  if (placed.empty() || maxFactor <= 1.0f) return;
  // Offsets to try beyond the five anchors, nearest first, so a bird that is
  // stopped on one side steps away from it by as little as will do.
  std::vector<std::pair<int, int>> shifts;
  if (nudge > 0) {
    const int step = std::max(1, nudge / 3);
    for (int dy = -nudge; dy <= nudge; dy += step)
      for (int dx = -nudge; dx <= nudge; dx += step)
        if (dx || dy) shifts.emplace_back(dx, dy);
    std::sort(shifts.begin(), shifts.end(), [](const auto &a, const auto &b) {
      return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
    });
  }
  // Each bird's name keeps the size the layout gave it - `namePx` is only the
  // fallback, for a placement that came from somewhere that set no size. Grow
  // the bird, not its name: a name that swelled with the bird would be chasing
  // the room it was just given.
  const auto pxFor = [&](size_t k) {
    if (labels.empty()) return 0;
    return std::max(kMinLabelPx, placed[k].labelPx > 0 ? placed[k].labelPx : namePx);
  };

  // Every bird's sprite as placed, and one grid of all of them. Each bird is
  // lifted out of the grid while it is tried and put back where it lands, so
  // "everyone but me" costs one sprite's worth of work rather than a rebuild
  // from every other bird.
  const size_t n = placed.size();
  std::vector<Sprite> sprites(n);
  Mask occ(boxW, boxH);
  for (size_t k = 0; k < n; ++k) {
    sprites[k] = spriteAt(sources, flips, labels, placed[k].index, placed[k].dim, pxFor(k));
    stamp(occ, sprites[k].mask, placed[k].x - sprites[k].artX, placed[k].y - sprites[k].artY);
  }

  // Smallest first: they have the most to gain, and a big bird growing first
  // would take the room a small one beside it needed.
  std::vector<size_t> order(n);
  for (size_t k = 0; k < n; ++k) order[k] = k;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return placed[a].dim < placed[b].dim; });

  // The size every bird started at, so `maxFactor` stays a bound on the whole
  // growth however many rounds it takes to get there.
  std::vector<int> fromDim(n);
  for (size_t k = 0; k < n; ++k) fromDim[k] = placed[k].dim;

  // Each round re-bases every bird on where it now stands: a bird that stepped
  // aside last round can step again from its new place, and one that was held
  // by a neighbour can grow once that neighbour has moved. Rounds stop early
  // when nothing moved, so the cost is paid only while it is buying something.
  for (int round = 0; round < std::max(1, rounds); ++round) {
  bool moved = false;
  for (size_t k : order) {
    const Sprite &cur = sprites[k];
    const int sx = placed[k].x - cur.artX, sy = placed[k].y - cur.artY;
    const int sw = cur.mask.width(), sh = cur.mask.height();

    // What is left of the budget at this bird's current size.
    float cap = float(fromDim[k]) * maxFactor / float(std::max(1, placed[k].dim));
    if (roundStep > 1.0f) cap = std::min(cap, roundStep);  // this round's share only
    // The least growth worth a redraw: two pixels on the longest side or one
    // percent, whichever is more. Below that it is only the rounding moving.
    const int minDim = std::max(placed[k].dim + 2, int(std::ceil(placed[k].dim * 1.01f)));
    const int maxDim = int(std::lround(placed[k].dim * cap));
    if (maxDim < minDim) continue;

    unstamp(occ, cur.mask, sx, sy);
    // A candidate is tried with its centre where the bird's is, then holding
    // each edge, since the room is often to one side, then shifted.
    auto tryAt = [&](int dim, Sprite &out, int &ox, int &oy) -> bool {
      Sprite cand = spriteAt(sources, flips, labels, placed[k].index, dim, pxFor(k));
      const int cw = cand.mask.width(), ch = cand.mask.height();
      const int tries[5][2] = {
          {sx + (sw - cw) / 2, sy + (sh - ch) / 2},  // centre held
          {sx, sy + (sh - ch) / 2},                  // left edge held
          {sx + sw - cw, sy + (sh - ch) / 2},        // right
          {sx + (sw - cw) / 2, sy},                  // top
          {sx + (sw - cw) / 2, sy + sh - ch},        // bottom
      };
      for (const auto &t : tries) {
        if (fitsAt(occ, cand.mask, t[0], t[1], boxW, boxH)) {
          out = std::move(cand);
          ox = t[0], oy = t[1];
          return true;
        }
      }
      // Then, shifted from where it stands: the room may be off to one side.
      for (const auto &[dx, dy] : shifts) {
        const int x = sx + (sw - cw) / 2 + dx, y = sy + (sh - ch) / 2 + dy;
        if (fitsAt(occ, cand.mask, x, y, boxW, boxH)) {
          out = std::move(cand);
          ox = x, oy = y;
          return true;
        }
      }
      return false;
    };

    // The least growth first. A bird boxed in on every side - most of them,
    // a few rounds in - fails here once, where a bisection from the top
    // would have failed at every size it tried on the way down.
    Sprite best;
    int bx = 0, by = 0;
    if (!tryAt(minDim, best, bx, by)) {
      stamp(occ, cur.mask, sx, sy);
      continue;
    }
    // Then bisect between that and the cap, to within a percent: finer than
    // that, each step buys a pixel or two for another scale and erode.
    int lo = minDim, hi = maxDim + 1;  // lo fits; hi is not known to
    while (hi - lo > std::max(1, lo / 100)) {
      const int mid = (lo + hi) / 2;
      Sprite cand;
      int cx = 0, cy = 0;
      if (tryAt(mid, cand, cx, cy)) {
        lo = mid;
        best = std::move(cand);
        bx = cx, by = cy;
      } else {
        hi = mid;
      }
    }
    moved = true;
    stamp(occ, best.mask, bx, by);
    placed[k].dim = best.dim;
    placed[k].x = bx + best.artX;
    placed[k].y = by + best.artY;
    placed[k].labelX = bx + best.labelX;
    placed[k].labelY = by + best.labelY;
    placed[k].labelW = best.labelW;
    placed[k].labelH = best.labelH;
    placed[k].labelPx = best.labelPx;
    sprites[k] = std::move(best);
  }
  if (!moved) break;
  }
}

}  // namespace birdposter
