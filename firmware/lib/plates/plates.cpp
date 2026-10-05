#pragma GCC optimize("O2")  // sprite expansion is on every bird drawn; the build is -Os
#include "plates.h"

#include <cstring>

#include "planecoder.h"

namespace birdposter {

namespace {

constexpr uint32_t kMagic = 0x4C504746u;  // 'FGPL'
constexpr uint32_t kVersion = 7;

// Little-endian field readers over a byte buffer; the pack is written that way
// and both targets are little-endian, but reading bytes keeps alignment out of
// the picture.
struct Cursor {
  const uint8_t *p;
  const uint8_t *end;
  bool ok = true;

  bool need(size_t n) {
    if (size_t(end - p) < n) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? *p++ : 0; }
  uint16_t u16() {
    if (!need(2)) return 0;
    const uint16_t v = uint16_t(p[0] | (p[1] << 8));
    p += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    const uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                       (uint32_t(p[3]) << 24);
    p += 4;
    return v;
  }
  float f32() {
    const uint32_t bits = u32();
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
  }
};

bool readStream(const Reader &reader, uint32_t offset, uint32_t len, std::vector<uint8_t> &out) {
  out.resize(len);
  return reader(offset, out.data(), len);
}

}  // namespace

bool Plates::open(Reader reader, std::string *error) {
  reader_ = std::move(reader);
  entries_.clear();

  // Header: 4+4+1+1+2+4 = 16, then the paper tone.
  uint8_t head[16 + 3];
  if (!reader_(0, head, sizeof head)) {
    if (error) *error = "pack too short";
    return false;
  }
  Cursor c{head, head + sizeof head};
  const uint32_t magic = c.u32(), version = c.u32();
  if (magic != kMagic || version != kVersion) {
    if (error) *error = "not a v7 FGPL pack (re-run bake_plates.py)";
    return false;
  }
  const int depth = c.u8();
  const int block = c.u8();
  source_ = c.u16();
  const uint32_t count = c.u32();
  std::memcpy(paper_, c.p, 3);
  if (depth != 4 || block != kChromaBlock) {
    if (error) *error = "unsupported code depth or chroma block";
    return false;
  }

  // The index is variable-length (names), so read it record by record. A
  // record is at most 2 + 255 + 4 + 2 + 4 + 48 + 12 bytes.
  constexpr size_t kFixed = 4 + 2 + 4 + 48 + 12;
  uint32_t at = sizeof head;
  entries_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t buf[2 + 255 + kFixed];
    uint16_t nameLen = 0;
    if (!reader_(at, &nameLen, 2)) break;
    const size_t recLen = 2 + nameLen + kFixed;
    if (nameLen > 255 || !reader_(at, buf, recLen)) break;
    Cursor r{buf, buf + recLen};
    PlateEntry e;
    r.u16();
    e.name.assign(reinterpret_cast<const char *>(r.p), nameLen);
    r.p += nameLen;
    e.w = r.u16();
    e.h = r.u16();
    e.flip = r.u8() != 0;
    e.alias = r.u8() != 0;
    e.label.w = r.u16();
    e.label.h = r.u16();
    std::memcpy(e.luma, r.p, 16);
    r.p += 16;
    std::memcpy(e.cb, r.p, 16);
    r.p += 16;
    std::memcpy(e.cr, r.p, 16);
    r.p += 16;
    e.offset = r.u32();
    e.lumaLen = r.u32();
    e.chromaLen = r.u32();
    if (!r.ok) break;
    entries_.push_back(std::move(e));
    at += recLen;
  }
  payload_ = at;
  if (entries_.size() != count) {
    if (error) *error = "pack index truncated";
    return false;
  }
  return true;
}

int Plates::find(const std::string &scientific) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].name == scientific) return int(i);
  return -1;
}

bool Plates::loadMask(size_t i, Mask &out, std::vector<uint8_t> *keep) const {
  const PlateEntry &e = entries_[i];
  const size_t px = size_t(e.w) * e.h;
  std::vector<uint8_t> stream, luma(px);
  if (!readStream(reader_, payload_ + e.offset, e.lumaLen, stream)) return false;
  if (!planecoder::decodeLuma(stream.data(), stream.size(), e.w, e.h, luma.data())) return false;
  out = Mask(e.w, e.h);
  for (int y = 0; y < e.h; ++y)
    for (int x = 0; x < e.w; ++x)
      if (luma[size_t(y) * e.w + x] != kOutside) out.set(x, y);
  if (keep) {
    // Codes are four bits: two a byte, the first in the high nibble.
    keep->assign((px + 1) / 2, 0);
    for (size_t k = 0; k < px; ++k) (*keep)[k / 2] |= uint8_t(luma[k] << (k & 1 ? 0 : 4));
  }
  return true;
}

bool Plates::loadSprite(size_t i, SpriteImage &out, const std::vector<uint8_t> *kept) const {
  return decode(entries_[i], reader_, payload_ + entries_[i].offset, out, kept);
}

bool Plates::decodeSingle(const std::string &file, SpriteImage &out, std::string *error) {
  // magic, version, block u8, source u16, w u16, h u16, three 16-byte tables,
  // two u32 stream lengths: 4 + 4 + 1 + 2 + 4 + 48 + 8 = 71.
  constexpr size_t kHead = 71;
  const auto *data = reinterpret_cast<const uint8_t *>(file.data());
  Cursor c{data, data + file.size()};
  const uint32_t magic = c.u32(), version = c.u32();
  if (magic != 0x53504746u /* 'FGPS' */ || version != 2 || c.u8() != kChromaBlock) {
    if (error) *error = "not an FGPS v2 sprite";
    return false;
  }
  PlateEntry e;
  c.u16();  // the size it was baked at; the sprite's own w and h say it
  e.w = c.u16();
  e.h = c.u16();
  if (!c.need(48)) {
    if (error) *error = "sprite header short";
    return false;
  }
  std::memcpy(e.luma, c.p, 16);
  std::memcpy(e.cb, c.p + 16, 16);
  std::memcpy(e.cr, c.p + 32, 16);
  c.p += 48;
  e.lumaLen = c.u32();
  e.chromaLen = c.u32();
  if (!c.ok || e.w == 0 || e.h == 0 || file.size() < kHead + e.lumaLen + e.chromaLen) {
    if (error) *error = "sprite truncated";
    return false;
  }
  const Reader fromFile = [&file](uint32_t offset, void *dst, size_t len) {
    if (size_t(offset) + len > file.size()) return false;
    std::memcpy(dst, file.data() + offset, len);
    return true;
  };
  if (!decode(e, fromFile, kHead, out)) {
    if (error) *error = "sprite streams did not decode";
    return false;
  }
  return true;
}

bool Plates::decode(const PlateEntry &e, const Reader &reader, uint32_t base, SpriteImage &out,
                    const std::vector<uint8_t> *kept) {
  out.direct.clear();
  out.w = e.w;
  out.h = e.h;
  const size_t stride = (e.w + 7) / 8;
  const int bw = (e.w + kChromaBlock - 1) / kChromaBlock, bh = (e.h + kChromaBlock - 1) / kChromaBlock;
  const size_t px = size_t(e.w) * e.h, blocks = size_t(bw) * bh;

  // The luma code a pixel, straight from the coder, and the silhouette bit,
  // which is simply "not outside"; and which blocks have anything painted in
  // them. The chroma plane needs the luma plane to decode, so it comes second.
  std::vector<uint8_t> stream, chroma(blocks);
  out.bw = bw;
  out.bh = bh;
  out.luma.assign(px, kOutside);
  if (kept && kept->size() == (px + 1) / 2) {
    const uint8_t *k = kept->data();
    for (size_t j = 0; j < px; ++j) out.luma[j] = uint8_t(j & 1 ? k[j / 2] & 15 : k[j / 2] >> 4);
  } else if (!readStream(reader, base, e.lumaLen, stream) ||
             !planecoder::decodeLuma(stream.data(), stream.size(), e.w, e.h, out.luma.data())) {
    return false;
  }
  if (!readStream(reader, base + e.lumaLen, e.chromaLen, stream) ||
      !planecoder::decodeChroma(stream.data(), stream.size(), out.luma.data(), e.w, e.h,
                                kChromaBlock, chroma.data()))
    return false;
  out.paint.assign(stride * e.h, 0);
  std::vector<uint8_t> blockPainted(blocks, 0);
  for (int y = 0; y < e.h; ++y) {
    const size_t brow = size_t(y / kChromaBlock) * bw;
    const uint8_t *row = out.luma.data() + size_t(y) * e.w;
    uint8_t *bits = out.paint.data() + size_t(y) * stride;
    for (int x = 0; x < e.w; ++x) {
      if (row[x] == kOutside) continue;
      bits[x >> 3] |= uint8_t(0x80 >> (x & 7));
      blockPainted[brow + x / kChromaBlock] = 1;
    }
  }
  std::memcpy(out.lumaTable, e.luma, 16);

  // The chroma a block. An unpainted block is not in the file and decodes as
  // code 0, which is a colour of the bird's but not its neighbours'; it gets
  // the mean of the painted blocks around it instead, so a painted pixel next
  // to it blends towards its own kind. One pass is enough: a painted pixel's
  // four blocks are its own and its immediate neighbours.
  out.cb.assign(blocks, 0);
  out.cr.assign(blocks, 0);
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t code = chroma[i];
    out.cb[i] = e.cb[code];
    out.cr[i] = e.cr[code];
  }
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const size_t i = size_t(by) * bw + bx;
      if (blockPainted[i]) continue;
      int sumCb = 0, sumCr = 0, n = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = bx + dx, ny = by + dy;
          if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
          const size_t j = size_t(ny) * bw + nx;
          if (!blockPainted[j]) continue;
          sumCb += out.cb[j];
          sumCr += out.cr[j];
          ++n;
        }
      }
      if (n) {
        out.cb[i] = int8_t(sumCb / n);
        out.cr[i] = int8_t(sumCr / n);
      }
    }
  }
  return true;
}

}  // namespace birdposter
