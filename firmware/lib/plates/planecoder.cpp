#include "planecoder.h"

#include <algorithm>
#include <cstring>

#include "plane_prior.h"

namespace birdposter {
namespace planecoder {

namespace {

constexpr int kSymbols = 16;
constexpr uint8_t kOutside = 15;
constexpr uint32_t kInc = 24;
constexpr uint32_t kLimit = 1u << 14;
constexpr uint32_t kTop = 1u << 24;
constexpr int kLumaContexts = 256 * 5;
constexpr uint8_t kEdge = 16;  // a chroma neighbour off the sprite or unpainted
constexpr int kChromaContexts = 17 * 17 * 4;

struct Model {
  uint16_t f[kSymbols];
  uint32_t total;

  void update(int s) {
    f[s] = uint16_t(f[s] + kInc);
    total += kInc;
    if (total > kLimit) {
      uint32_t t = 0;
      for (uint16_t &v : f) {
        v = uint16_t((v + 1) >> 1);
        t += v;
      }
      total = t;
    }
  }
};

void startLuma(std::vector<Model> &models) {
  models.resize(kLumaContexts);
  for (int c = 0; c < kLumaContexts; ++c) {
    uint32_t t = 0;
    for (int i = 0; i < kSymbols; ++i) t += models[c].f[i] = kLumaPrior[c][i];
    models[c].total = t;
  }
}

void startChroma(std::vector<Model> &models) {
  models.resize(kChromaContexts);
  for (Model &m : models) {
    std::fill(std::begin(m.f), std::end(m.f), uint16_t(1));
    m.total = kSymbols;
  }
}

inline int slope(int d) { return d <= -3 ? 0 : d < 0 ? 1 : d == 0 ? 2 : d < 3 ? 3 : 4; }

class Decoder {
 public:
  Decoder(const uint8_t *data, size_t len) : p_(data), end_(data + len), read_(0) {
    for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | byte();
  }

  int decode(Model &m) {
    const uint32_t r = range_ / m.total;
    const uint32_t v = std::min(code_ / r, m.total - 1);
    int s = 0;
    uint32_t cum = 0;
    while (cum + m.f[s] <= v) cum += m.f[s++];
    code_ -= cum * r;
    range_ = m.f[s] * r;
    while (range_ < kTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | byte();
    }
    m.update(s);
    return s;
  }

  // Every byte read and none past the end: the stream was exactly this plane.
  bool exact(size_t len) const { return read_ == len; }

 private:
  uint8_t byte() {
    ++read_;
    return p_ < end_ ? *p_++ : 0;
  }
  const uint8_t *p_, *end_;
  size_t read_;
  uint32_t range_ = 0xFFFFFFFFu, code_ = 0;
};

class Encoder {
 public:
  void encode(Model &m, int s) {
    uint32_t cum = 0;
    for (int i = 0; i < s; ++i) cum += m.f[i];
    const uint32_t r = range_ / m.total;
    low_ += uint64_t(cum) * r;
    range_ = m.f[s] * r;
    while (range_ < kTop) {
      range_ <<= 8;
      shift();
    }
    m.update(s);
  }

  std::vector<uint8_t> finish() {
    for (int i = 0; i < 5; ++i) shift();
    return std::vector<uint8_t>(out_.begin() + 1, out_.end());  // the first is the empty cache
  }

 private:
  void shift() {
    if (low_ < 0xFF000000u || low_ >= (uint64_t(1) << 32)) {
      const uint8_t carry = uint8_t(low_ >> 32);
      uint8_t b = cache_;
      do {
        out_.push_back(uint8_t(b + carry));
        b = 0xFF;
      } while (--pending_);
      cache_ = uint8_t(low_ >> 24);
    }
    ++pending_;
    low_ = (low_ << 8) & 0xFFFFFFFFu;
  }
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint32_t pending_ = 1;
  std::vector<uint8_t> out_;
};

// Which blocks have anything painted in them, and each one's luma band: the
// integer mean of its painted codes, over 15 in four.
void blockLuma(const uint8_t *luma, int w, int h, int block, std::vector<uint8_t> &painted,
               std::vector<uint8_t> &band) {
  const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  std::vector<uint32_t> sum(size_t(bw) * bh, 0), n(size_t(bw) * bh, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t *row = luma + size_t(y) * w;
    const size_t brow = size_t(y / block) * bw;
    for (int x = 0; x < w; ++x) {
      if (row[x] == kOutside) continue;
      sum[brow + x / block] += row[x];
      ++n[brow + x / block];
    }
  }
  painted.assign(sum.size(), 0);
  band.assign(sum.size(), 0);
  for (size_t i = 0; i < sum.size(); ++i) {
    if (!n[i]) continue;
    painted[i] = 1;
    band[i] = uint8_t(std::min<uint32_t>(3, sum[i] / n[i] * 4 / 15));
  }
}

// The row buffers both luma directions walk: the row above and this one, each
// with an OUTSIDE pixel either side.
template <typename Step>
void walkLuma(int w, int h, Step step) {
  std::vector<uint8_t> rows(size_t(2) * (w + 2), kOutside);
  uint8_t *above = rows.data(), *row = rows.data() + w + 2;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int c = (above[x + 1] * 16 + row[x]) * 5 + slope(above[x + 2] - above[x]);
      row[x + 1] = step(y, x, c);
    }
    std::swap(above, row);
  }
}

template <typename Step>
void walkChroma(const std::vector<uint8_t> &painted, const std::vector<uint8_t> &band, int bw,
                int bh, Step step) {
  std::vector<uint8_t> rows(size_t(2) * (bw + 1), kEdge);
  uint8_t *above = rows.data(), *row = rows.data() + bw + 1;
  for (int y = 0; y < bh; ++y) {
    row[0] = kEdge;
    for (int x = 0; x < bw; ++x) {
      const size_t i = size_t(y) * bw + x;
      if (!painted[i]) {
        row[x + 1] = kEdge;
        continue;
      }
      row[x + 1] = step(i, (above[x + 1] * 17 + row[x]) * 4 + band[i]);
    }
    std::swap(above, row);
  }
}

}  // namespace

bool decodeLuma(const uint8_t *data, size_t len, int w, int h, uint8_t *out) {
  std::vector<Model> models;
  startLuma(models);
  Decoder dec(data, len);
  walkLuma(w, h, [&](int y, int x, int c) {
    const uint8_t s = uint8_t(dec.decode(models[size_t(c)]));
    out[size_t(y) * w + x] = s;
    return s;
  });
  return dec.exact(len);
}

bool decodeChroma(const uint8_t *data, size_t len, const uint8_t *luma, int w, int h, int block,
                  uint8_t *out) {
  const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  std::vector<uint8_t> painted, band;
  blockLuma(luma, w, h, block, painted, band);
  std::memset(out, 0, size_t(bw) * bh);
  std::vector<Model> models;
  startChroma(models);
  Decoder dec(data, len);
  walkChroma(painted, band, bw, bh, [&](size_t i, int c) {
    const uint8_t s = uint8_t(dec.decode(models[size_t(c)]));
    out[i] = s;
    return s;
  });
  return dec.exact(len);
}

std::vector<uint8_t> encodeLuma(const uint8_t *codes, int w, int h) {
  std::vector<Model> models;
  startLuma(models);
  Encoder enc;
  walkLuma(w, h, [&](int y, int x, int c) {
    const uint8_t s = codes[size_t(y) * w + x];
    enc.encode(models[size_t(c)], s);
    return s;
  });
  return enc.finish();
}

std::vector<uint8_t> encodeChroma(const uint8_t *chroma, const uint8_t *luma, int w, int h,
                                  int block) {
  const int bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  std::vector<uint8_t> painted, band;
  blockLuma(luma, w, h, block, painted, band);
  std::vector<Model> models;
  startChroma(models);
  Encoder enc;
  walkChroma(painted, band, bw, bh, [&](size_t i, int c) {
    enc.encode(models[size_t(c)], chroma[i]);
    return chroma[i];
  });
  return enc.finish();
}

}  // namespace planecoder
}  // namespace birdposter
