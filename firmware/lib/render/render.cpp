#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "qrcodegen.h"

// stb_truetype is a single header with the implementation in this one file.
// Static so nothing leaks out, which leaves every function it defines that we
// do not call "unused" - and there are many.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

namespace birdframe {

// dither.PALETTE_6: int(saturated * 0.5 + desaturated * 0.5), truncated the way
// Python's int() truncates. Order is the driver's: black, white, yellow, red,
// blue, green.
namespace {
constexpr uint8_t kInk[6][3] = {
    {0, 0, 0}, {208, 209, 210}, {231, 222, 35}, {205, 36, 37}, {30, 29, 174}, {29, 173, 35},
};
constexpr uint8_t scaled(int ink, int paper, int white) {
  const int v = (ink * paper + white / 2) / white;
  return uint8_t(v > 255 ? 255 : v);
}
constexpr uint8_t target(int i, int k) { return scaled(kInk[i][k], kPaperRgb[k], kInk[1][k]); }
}  // namespace

const uint8_t kInkRgb[6][3] = {
    {kInk[0][0], kInk[0][1], kInk[0][2]}, {kInk[1][0], kInk[1][1], kInk[1][2]},
    {kInk[2][0], kInk[2][1], kInk[2][2]}, {kInk[3][0], kInk[3][1], kInk[3][2]},
    {kInk[4][0], kInk[4][1], kInk[4][2]}, {kInk[5][0], kInk[5][1], kInk[5][2]},
};
const uint8_t kInkTarget[6][3] = {
    {target(0, 0), target(0, 1), target(0, 2)}, {target(1, 0), target(1, 1), target(1, 2)},
    {target(2, 0), target(2, 1), target(2, 2)}, {target(3, 0), target(3, 1), target(3, 2)},
    {target(4, 0), target(4, 1), target(4, 2)}, {target(5, 0), target(5, 1), target(5, 2)},
};

void Canvas::reset(int width, int height, uint16_t fill) {
  w = width;
  h = height;
  px.assign(size_t(w) * h, fill);
}

void Frame::reset(int width, int height, Ink fill) {
  w = width;
  h = height;
  px.assign(size_t(w) * h, fill);
}

void Frame::fillRect(int x, int y, int rw, int rh, Ink ink) {
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(w, x + rw), y1 = std::min(h, y + rh);
  for (int yy = y0; yy < y1; ++yy) std::memset(row(yy) + x0, ink, size_t(std::max(0, x1 - x0)));
}

namespace {

// The Mitchell-Netravali family at distance `x` from the sample, as a weight.
float cubic(float x, float B, float C) {
  x = std::fabs(x);
  if (x < 1)
    return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
  if (x < 2)
    return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x +
            (8 * B + 24 * C)) / 6;
  return 0;
}

float kernel(Resample r, float x) {
  switch (r) {
    case Resample::Mitchell: return cubic(x, 1.0f / 3, 1.0f / 3);
    case Resample::CatmullRom: return cubic(x, 0, 0.5f);
    case Resample::Bilinear: break;
  }
  x = std::fabs(x);
  return x < 1 ? 1 - x : 0;
}

}  // namespace

void drawSprite(Canvas &canvas, const SpriteImage &sprite, int x, int y, int dstW, int dstH,
                bool flip, Resample resample) {
  if (dstW <= 0 || dstH <= 0 || sprite.w <= 0 || sprite.h <= 0) return;
  const int dx0 = std::max(0, -x), dx1 = std::min(dstW, canvas.w - x);

  // Shrinking by less than 2x, or growing: filtered. Growing is the flash
  // pack's case (448 px baked, drawn larger on a sparse page), and a plain
  // pixel-a-pixel copy leaves each plate pixel a visible block that the dither
  // does not hide once a bird is drawn three times over. Whether a pixel is
  // painted still comes from the nearest plate pixel, so the outline is the
  // mask the packer placed; its colour is the plate pixels around it, painted
  // ones only, so the page never bleeds into a bird's edge.
  if (dstW * 2 > sprite.w && dstH * 2 > sprite.h) {
    // Per column (and per row) once: the nearest plate pixel, and the four
    // either side of the pixel's centre with their weights in 1/1024ths.
    // Bilinear's outer two weigh nothing and are skipped.
    struct Tap { int nearest; int at[4]; int w[4]; };
    const auto taps = [resample](int dst, int src, bool mirror) {
      std::vector<Tap> t(dst);
      for (int d = 0; d < dst; ++d) {
        Tap &tap = t[d];
        const int n = int((int64_t(d) * src) / dst);
        tap.nearest = mirror ? src - 1 - n : n;
        float pos = (d + 0.5f) * src / dst - 0.5f;  // the centre, in plate pixels
        if (mirror) pos = (src - 1) - pos;
        const int i0 = int(std::floor(pos));
        const float f = pos - i0;
        for (int k = 0; k < 4; ++k) {
          tap.at[k] = std::clamp(i0 - 1 + k, 0, src - 1);
          tap.w[k] = int(std::lround(kernel(resample, f - (k - 1)) * 1024));
        }
      }
      return t;
    };
    const std::vector<Tap> cols = taps(dstW, sprite.w, flip);
    const std::vector<Tap> rows = taps(dstH, sprite.h, false);
    for (int dy = 0; dy < dstH; ++dy) {
      const int cy = y + dy;
      if (cy < 0 || cy >= canvas.h) continue;
      const Tap &ty = rows[dy];
      uint16_t *out = canvas.row(cy) + x;
      for (int dx = dx0; dx < dx1; ++dx) {
        const Tap &tx = cols[dx];
        if (!sprite.painted(tx.nearest, ty.nearest)) continue;
        int64_t r = 0, g = 0, b = 0, sum = 0;
        for (int j = 0; j < 4; ++j) {
          if (!ty.w[j]) continue;
          for (int i = 0; i < 4; ++i) {
            if (!tx.w[i] || !sprite.painted(tx.at[i], ty.at[j])) continue;
            const int64_t w = int64_t(tx.w[i]) * ty.w[j];
            const uint16_t c = sprite.at(tx.at[i], ty.at[j]);
            r += w * ((c >> 11) & 31);
            g += w * ((c >> 5) & 63);
            b += w * (c & 31);
            sum += w;
          }
        }
        // A cubic's weights go negative; with most of the neighbourhood
        // unpainted, what is left can sum to almost nothing. The nearest
        // pixel is the honest answer there.
        if (sum < (int64_t(1) << 20) / 4) {
          out[dx] = sprite.at(tx.nearest, ty.nearest);
          continue;
        }
        const auto ch = [sum](int64_t v, int hi) {
          return int(std::clamp<int64_t>((v + sum / 2) / sum, 0, hi));
        };
        out[dx] = uint16_t(ch(r, 31) << 11 | ch(g, 63) << 5 | ch(b, 31));
      }
    }
    return;
  }

  // Shrinking by 2x or more - the SD card's full-size pack drawn at page
  // size - is a box filter: every destination pixel is the mean of the
  // source pixels it covers, painted ones only, and it is painted when at
  // least half of them are. Sampling one of nine pixels would throw the
  // card's resolution away and alias every feather edge into a moiré.
  std::vector<int> cx0(dstW + 1);
  for (int dx = 0; dx <= dstW; ++dx) cx0[dx] = int((int64_t(dx) * sprite.w) / dstW);
  for (int dy = 0; dy < dstH; ++dy) {
    const int cy = y + dy;
    if (cy < 0 || cy >= canvas.h) continue;
    const int sy0 = int((int64_t(dy) * sprite.h) / dstH);
    const int sy1 = std::max(sy0 + 1, int((int64_t(dy + 1) * sprite.h) / dstH));
    uint16_t *out = canvas.row(cy) + x;
    for (int dx = dx0; dx < dx1; ++dx) {
      const int fx = flip ? dstW - 1 - dx : dx;
      const int sx0 = cx0[fx], sx1 = std::max(sx0 + 1, cx0[fx + 1]);
      int r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1; ++sy) {
        for (int sx = sx0; sx < sx1; ++sx) {
          if (!sprite.painted(sx, sy)) continue;
          const uint16_t c = sprite.at(sx, sy);
          r += (c >> 11) & 31;
          g += (c >> 5) & 63;
          b += c & 31;
          ++n;
        }
      }
      if (n * 2 < (sy1 - sy0) * (sx1 - sx0)) continue;
      out[dx] = uint16_t(((r / n) << 11) | ((g / n) << 5) | (b / n));
    }
  }
}

namespace {

// Which ink is nearest is decided in luma and chroma, with chroma weighted
// three times: in plain RGB a mid grey is about as close to the green ink as
// to black, and a grey wash dithers into green, red and blue speckle rather
// than black dots. Weighted, a grey chooses between black and white only,
// and a colour has to be a colour before a coloured ink gets it.
constexpr int kChromaWeight = 3;

struct Ycc {
  int y, cb, cr;
};
inline Ycc toYcc(int r, int g, int b) {
  const int y = (77 * r + 151 * g + 28 * b) >> 8;
  return Ycc{y, b - y, r - y};
}
struct InkYcc {
  Ycc v[6];
  InkYcc() {
    for (int i = 0; i < 6; ++i) v[i] = toYcc(kInkTarget[i][0], kInkTarget[i][1], kInkTarget[i][2]);
  }
};
const InkYcc kInkYcc;

inline uint8_t nearestInk(int r, int g, int b) {
  const Ycc p = toYcc(r, g, b);
  int best = 0, bestD = 1 << 30;
  for (int i = 0; i < 6; ++i) {
    const int dy = p.y - kInkYcc.v[i].y, dcb = p.cb - kInkYcc.v[i].cb, dcr = p.cr - kInkYcc.v[i].cr;
    const int d = dy * dy + kChromaWeight * (dcb * dcb + dcr * dcr);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return uint8_t(best);
}

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Saturation is stretched about each colour's own luma, so a grey stays the
// grey it was. Contrast pivots on the paper, so the page's white holds still
// and it is everything below it - a white bird's shading included - that
// deepens: a punchier print, not a brighter one.
struct Vivid {
  float saturation;
  float contrast;
};
constexpr Vivid kVivid[kVividLevels] = {
    {1.0f, 1.0f}, {1.25f, 1.1f}, {1.5f, 1.2f}, {1.8f, 1.3f}, {2.2f, 1.4f}};
constexpr float kContrastPivot =
    0.299f * kPaperRgb[0] + 0.587f * kPaperRgb[1] + 0.114f * kPaperRgb[2];  // the paper's luma

// The paper is the white point, and it is warm, not grey: stretched about a
// neutral luma it would drift, and a paper that is no longer exactly the
// white target dithers into stray dots. So the work is done with the paper
// normalised to neutral - each channel scaled so paper reads as one grey -
// and scaled back after, which leaves the paper precisely where it was.
// One entry per RGB565 value, three bytes each: 192 KB, built per page.
void boostTable(int level, std::vector<uint8_t> &out) {
  const Vivid v = kVivid[level < 0 ? 0 : (level >= kVividLevels ? kVividLevels - 1 : level)];
  float norm[3];
  for (int k = 0; k < 3; ++k) norm[k] = kContrastPivot / float(kPaperRgb[k]);
  out.resize(65536 * 3);
  for (int i = 0; i < 65536; ++i) {
    int rgb[3];
    rgb888(uint16_t(i), rgb[0], rgb[1], rgb[2]);
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = float(rgb[k]) * norm[k];
    const float luma = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    for (int k = 0; k < 3; ++k) {
      const float saturated = luma + (c[k] - luma) * v.saturation;
      const float stretched = kContrastPivot + (saturated - kContrastPivot) * v.contrast;
      out[size_t(i) * 3 + k] = uint8_t(clamp255(int(stretched / norm[k] + 0.5f)));
    }
  }
}

// Unsharp amount in sixteenths: p + a/16 * (p - mean of the 3x3 around it).
constexpr int kSharpen16[kSharpenLevels] = {0, 4, 8, 12, 16};

// Edge ink: the pixel is darkened by gain * 255 * sqrt(mag / 255) * luma/255,
// mag being the Sobel gradient of luma scaled to 0-255. The square root is
// the point - a faint edge (mag 30) darkens by a third at gain 1, a strong one
// goes to black - the luma factor keeps it to the light, and the gain is in
// 64ths.
constexpr int kEdgeGain64[kEdgeLevels] = {0, 24, 40, 56, 72};

// One canvas row as boosted RGB, its horizontal 3-sum and its luma, all
// padded a pixel each side by edge replication so the vertical pass needs no
// tests.
struct RgbRow {
  std::vector<int16_t> rgb, hsum, luma;
  void init(int w) {
    rgb.assign(size_t(w + 2) * 3, 0);
    hsum.assign(size_t(w + 2) * 3, 0);
    luma.assign(size_t(w + 2), 0);
  }
  void load(const uint16_t *src, int w, const uint8_t *table) {
    for (int x = 0; x < w; ++x) {
      const uint8_t *c = table + size_t(src[x]) * 3;
      int16_t *d = &rgb[size_t(x + 1) * 3];
      d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
      luma[size_t(x + 1)] = int16_t((77 * c[0] + 151 * c[1] + 28 * c[2]) >> 8);
    }
    for (int k = 0; k < 3; ++k) {
      rgb[k] = rgb[3 + k];
      rgb[size_t(w + 1) * 3 + k] = rgb[size_t(w) * 3 + k];
    }
    luma[0] = luma[1];
    luma[size_t(w + 1)] = luma[size_t(w)];
    for (int x = 1; x <= w; ++x)
      for (int k = 0; k < 3; ++k)
        hsum[size_t(x) * 3 + k] = int16_t(rgb[size_t(x - 1) * 3 + k] + rgb[size_t(x) * 3 + k] +
                                          rgb[size_t(x + 1) * 3 + k]);
  }
};

// 255 * sqrt(m / 255) for m in 0..255, so the edge curve is a lookup.
struct EdgeCurve {
  uint8_t v[256];
  EdgeCurve() {
    for (int m = 0; m < 256; ++m) v[m] = uint8_t(int(255.0 * std::sqrt(m / 255.0) + 0.5));
  }
};
const EdgeCurve kEdgeCurve;

}  // namespace

void dither(const Canvas &canvas, Frame &out, int vivid, int sharpen, int edges, int jitter) {
  out.reset(canvas.w, canvas.h, kWhite);
  const int w = canvas.w, h = canvas.h;
  std::vector<uint8_t> boostedTable;
  boostTable(vivid, boostedTable);
  const uint8_t *boosted = boostedTable.data();
  const int a16 = kSharpen16[sharpen < 0 ? 0 : (sharpen >= kSharpenLevels ? kSharpenLevels - 1 : sharpen)];
  const int g64 = kEdgeGain64[edges < 0 ? 0 : (edges >= kEdgeLevels ? kEdgeLevels - 1 : edges)];
  // Three canvas rows as RGB - above, this, below - rolling down the page.
  RgbRow rows[3];
  for (RgbRow &r : rows) r.init(w);
  rows[0].load(canvas.row(0), w, boosted);  // row -1 replicates row 0
  rows[1].load(canvas.row(0), w, boosted);
  int above = 0, here = 1, below = 2;
  // Two rows of error, three channels, with a pixel of slack each side so the
  // x-1 / x+1 taps need no bounds test.
  std::vector<int16_t> errA((w + 2) * 3, 0), errB((w + 2) * 3, 0);
  int16_t *cur = errA.data(), *next = errB.data();
  // Error diffusion starts with no error in hand, and on a flat light tone
  // it takes a hundred rows to settle: the top of the page comes out blank
  // for a band, then in regular lines of dots. So the first row is dithered
  // this many times over before the page proper and the result thrown away,
  // and the page starts with the error it would have carried anyway.
  constexpr int kWarmRows = 64;
  std::vector<uint8_t> scratch(static_cast<size_t>(w));
  for (int y = -kWarmRows; y < h; ++y) {
    rows[below].load(canvas.row(std::clamp(y + 1, 0, h - 1)), w, boosted);
    std::fill(next, next + (w + 2) * 3, 0);
    uint8_t *dst = y < 0 ? scratch.data() : out.row(y);
    const int16_t *p = rows[here].rgb.data();
    const int16_t *sa = rows[above].hsum.data(), *sh = rows[here].hsum.data(),
                  *sb = rows[below].hsum.data();
    const int16_t *la = rows[above].luma.data(), *lh = rows[here].luma.data(),
                  *lb = rows[below].luma.data();
    // Serpentine: alternate rows run right to left, so the error's forward
    // bias flips each row and the diagonal worms plain Floyd-Steinberg draws
    // through flat colour cancel instead of compounding.
    const bool rtl = y & 1;
    const int step = rtl ? -1 : 1;
    for (int x = rtl ? w - 1 : 0; x >= 0 && x < w; x += step) {
      const size_t i = size_t(x + 1) * 3;
      int rgb[3];
      for (int k = 0; k < 3; ++k) {
        const int v = p[i + k];
        // p + a * (p - mean9) with mean9 = sum9 / 9, in integers.
        rgb[k] = a16 ? v + (a16 * (9 * v - (sa[i + k] + sh[i + k] + sb[i + k]))) / (16 * 9) : v;
      }
      if (g64) {
        // Sobel on luma, L1 magnitude, scaled so a full black-white step is 255.
        const size_t c = size_t(x + 1);
        const int gx = (la[c + 1] + 2 * lh[c + 1] + lb[c + 1]) - (la[c - 1] + 2 * lh[c - 1] + lb[c - 1]);
        const int gy = (lb[c - 1] + 2 * lb[c] + lb[c + 1]) - (la[c - 1] + 2 * la[c] + la[c + 1]);
        const int mag = std::min(255, (std::abs(gx) + std::abs(gy)) / 8);
        // Weighted by the pixel's own lightness: a line on a white bird is
        // the whole point, a line through dark plumage is only mud. It also
        // puts the line on the light side of a light/dark boundary.
        // ... and applied as a scale toward black rather than a subtraction:
        // taking the same amount off every channel would leave the pixel
        // cooler than the warm paper, and its error would land as blue dots
        // rather than a black line.
        const int dark = (g64 * kEdgeCurve.v[mag] * lh[c]) / (64 * 255);
        for (int k = 0; k < 3; ++k) rgb[k] = rgb[k] * (255 - dark) / 255;
      }
      int16_t *e = cur + (x + 1) * 3;
      // Clamp before choosing, and take the error from the clamped value, so a
      // page of paper - brighter than the panel's white - does not accumulate
      // an unbounded debt that flips a pixel a hundred rows later.
      const int r = clamp255(rgb[0] + e[0]), g = clamp255(rgb[1] + e[1]),
                b = clamp255(rgb[2] + e[2]);
      uint8_t ink;
      if (jitter) {
        // One nudge for all three channels - lighter or darker, not a colour
        // shift - from a hash of the position, so a page is repeatable.
        uint32_t hsh = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
        hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
        const int n = int((hsh ^ (hsh >> 16)) & 255) - 128;
        const int d = n * jitter / 128;
        ink = nearestInk(clamp255(r + d), clamp255(g + d), clamp255(b + d));
      } else {
        ink = nearestInk(r, g, b);
      }
      dst[x] = ink;
      const int er = r - kInkTarget[ink][0], eg = g - kInkTarget[ink][1],
                eb = b - kInkTarget[ink][2];
      // "Ahead" and "behind" follow the walk; the row below gets the same
      // three taps mirrored.
      int16_t *ahead = cur + (x + 1 + step) * 3;
      int16_t *dBehind = next + (x + 1 - step) * 3, *d = next + (x + 1) * 3,
              *dAhead = next + (x + 1 + step) * 3;
      ahead[0] += er * 7 / 16;
      ahead[1] += eg * 7 / 16;
      ahead[2] += eb * 7 / 16;
      dBehind[0] += er * 3 / 16;
      dBehind[1] += eg * 3 / 16;
      dBehind[2] += eb * 3 / 16;
      d[0] += er * 5 / 16;
      d[1] += eg * 5 / 16;
      d[2] += eb * 5 / 16;
      dAhead[0] += er / 16;
      dAhead[1] += eg / 16;
      dAhead[2] += eb / 16;
    }
    std::swap(cur, next);
    const int spent = above;
    above = here;
    here = below;
    below = spent;
  }
}

// --- text -------------------------------------------------------------------

namespace {

const stbtt_fontinfo *fontInfo(const std::vector<uint8_t> &info) {
  return reinterpret_cast<const stbtt_fontinfo *>(info.data());
}

// Minimal UTF-8: names are ASCII, but an SSID on the setup page may not be.
int nextCodepoint(const std::string &s, size_t &i) {
  const unsigned char c = s[i++];
  if (c < 0x80) return c;
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
  int cp = c & (0x3F >> n);
  while (n-- > 0 && i < s.size()) cp = (cp << 6) | (s[i++] & 0x3F);
  return cp;
}

}  // namespace

bool Font::load(std::vector<uint8_t> bytes) {
  bytes_ = std::move(bytes);
  info_.assign(sizeof(stbtt_fontinfo), 0);
  auto *info = reinterpret_cast<stbtt_fontinfo *>(info_.data());
  if (!stbtt_InitFont(info, bytes_.data(), stbtt_GetFontOffsetForIndex(bytes_.data(), 0))) {
    info_.clear();
    return false;
  }
  return true;
}

bool Font::covers(const std::string &text) const {
  if (!ok()) return false;
  const stbtt_fontinfo *info = fontInfo(info_);
  for (size_t i = 0; i < text.size();)
    if (!stbtt_FindGlyphIndex(info, nextCodepoint(text, i))) return false;
  return true;
}

int Font::measure(const std::string &text, int px, int *ascent, int *descent) const {
  if (!ok()) return 0;
  const stbtt_fontinfo *info = fontInfo(info_);
  const float scale = stbtt_ScaleForMappingEmToPixels(info, float(px));
  int asc = 0, desc = 0, gap = 0;
  stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
  if (ascent) *ascent = int(asc * scale + 0.5f);
  if (descent) *descent = int(-desc * scale + 0.5f);
  float width = 0;
  int prev = 0;
  for (size_t i = 0; i < text.size();) {
    const int cp = nextCodepoint(text, i);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
    if (prev) width += stbtt_GetCodepointKernAdvance(info, prev, cp) * scale;
    width += adv * scale;
    prev = cp;
  }
  return int(width + 0.5f);
}

void Font::draw(Frame &frame, const std::string &text, int x, int baseline, int px,
                Ink ink) const {
  if (!ok()) return;
  const stbtt_fontinfo *info = fontInfo(info_);
  const float scale = stbtt_ScaleForMappingEmToPixels(info, float(px));
  float pen = float(x);
  int prev = 0;
  std::vector<uint8_t> bitmap;
  for (size_t i = 0; i < text.size();) {
    const int cp = nextCodepoint(text, i);
    if (prev) pen += stbtt_GetCodepointKernAdvance(info, prev, cp) * scale;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(info, cp, scale, scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
      bitmap.assign(size_t(gw) * gh, 0);
      stbtt_MakeCodepointBitmap(info, bitmap.data(), gw, gh, gw, scale, scale, cp);
      const int ox = int(pen) + x0, oy = baseline + y0;
      const auto at = [&](int gx, int gy) -> int {
        return gx < 0 || gy < 0 || gx >= gw || gy >= gh ? 0 : bitmap[size_t(gy) * gw + gx];
      };
      // The peak across a thin line, `a` and `b` its neighbours either side.
      // Of two equal pixels only the second wins, so a line split evenly
      // across two still lands one wide; and it must carry a quarter of a
      // pixel between it and its neighbour, or it is antialiasing, not a line.
      const auto peak = [](int c, int a, int b) {
        return a < 128 && b < 128 && c >= a && c > b && c + std::max(a, b) >= 64;
      };
      for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
          const int c = at(gx, gy);
          if (c >= 128 ||
              (keepHairlines_ && c > 0 &&
               (peak(c, at(gx - 1, gy), at(gx + 1, gy)) || peak(c, at(gx, gy - 1), at(gx, gy + 1)))))
            frame.set(ox + gx, oy + gy, ink);
        }
    }
    pen += adv * scale;
    prev = cp;
  }
}

void drawCentred(Frame &frame, const Font &font, const std::string &text, int boxX, int boxY,
                 int boxW, int boxH, int px, Ink ink) {
  int ascent = 0, descent = 0;
  const int width = font.measure(text, px, &ascent, &descent);
  const int x = boxX + (boxW - width) / 2;
  const int baseline = boxY + (boxH - (ascent + descent)) / 2 + ascent;
  font.draw(frame, text, x, baseline, px, ink);
}

// --- QR ---------------------------------------------------------------------

namespace {

constexpr int kQuiet = 4;  // modules of margin the standard asks for

bool encode(const std::string &text, std::vector<uint8_t> &qr) {
  std::vector<uint8_t> temp(qrcodegen_BUFFER_LEN_MAX);
  qr.assign(qrcodegen_BUFFER_LEN_MAX, 0);
  return qrcodegen_encodeText(text.c_str(), temp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
                              true);
}

}  // namespace

int qrSize(const std::string &text, int module) {
  std::vector<uint8_t> qr;
  if (!encode(text, qr)) return 0;
  return (qrcodegen_getSize(qr.data()) + 2 * kQuiet) * module;
}

int drawQr(Frame &frame, const std::string &text, int x, int y, int module) {
  std::vector<uint8_t> qr;
  if (!encode(text, qr)) return 0;
  const int n = qrcodegen_getSize(qr.data());
  const int side = (n + 2 * kQuiet) * module;
  frame.fillRect(x, y, side, side, kWhite);
  for (int my = 0; my < n; ++my)
    for (int mx = 0; mx < n; ++mx)
      if (qrcodegen_getModule(qr.data(), mx, my))
        frame.fillRect(x + (mx + kQuiet) * module, y + (my + kQuiet) * module, module, module,
                       kBlack);
  return side;
}

std::string wifiQrPayload(const std::string &ssid, const std::string &pass) {
  // Backslash-escape the characters the format reserves.
  auto esc = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
      out += c;
    }
    return out;
  };
  if (pass.empty()) return "WIFI:T:nopass;S:" + esc(ssid) + ";;";
  return "WIFI:T:WPA;S:" + esc(ssid) + ";P:" + esc(pass) + ";;";
}

}  // namespace birdframe
