// Reader for the plate pack `tools/bake_plates.py` writes (FGPL v6).
//
// The pack is the SD card's replacement: one file in the flash filesystem that
// carries every species the frame can draw - silhouette, pixels, label
// box and flip. The index is read once into RAM (about 110 bytes a species)
// and the streams are pulled by offset when a bird is actually on the page,
// so a 550-species pack costs 60 KB to open, not 13 MB.
//
// Pixels come as two posterised planes, luma per pixel and chroma per 4x4
// block; luma code 15 means outside the silhouette, so the one plane is the
// sprite's shape for the packer and its pixels for the renderer. loadSprite()
// expands the planes; the colour of a pixel is made when it is drawn, with
// the chroma interpolated between blocks. See the bake tool's docstring for
// the layout.
//
// Arduino-free: the file is reached through `Reader`, which the device backs
// with LittleFS and the host harness with stdio, so the same code renders a
// page on a workstation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "packer.h"

namespace birdposter {

// Read `len` bytes at `offset` from the pack file. Returns false short.
using Reader = std::function<bool(uint32_t offset, void *dst, size_t len)>;

struct PlateEntry {
  std::string name;   // "Turdus merula" - the artwork key
  uint16_t w = 0, h = 0;          // sprite (= silhouette) size
  bool flip = false;              // the Python reference's blake2b answer
  bool alias = false;             // another spelling of a species already in the pack
  LabelBox label;                 // measured at kLabelRefPx
  uint8_t luma[16] = {0};         // Y per luma code; code 15 is outside
  int8_t cb[16] = {0}, cr[16] = {0};  // B - Y, R - Y per chroma code
  uint32_t offset = 0, lumaLen = 0, chromaLen = 0;
};

constexpr int kChromaBlock = 4;  // pixels a side per chroma sample, as baked
constexpr uint8_t kOutside = 15;  // the luma code that is not a level

// One sprite's pixels, expanded: a luma code a pixel, the silhouette bit a
// pixel, and the chroma a block - *not* a colour a pixel. The colour is made
// at `at()`, where the block's chroma is interpolated bilinearly between
// block centres: the bake stores one chroma value per 4x4 block, and
// repeating it over the block draws a visible 4-pixel grid across every
// colour edge, while a value blended from the four nearest blocks reads as
// the wash it was. Blocks with nothing painted in them are given their
// painted neighbours' mean on load, so the blend at the silhouette's edge
// leans on the bird and not on whatever code 0 happened to be.
struct SpriteImage {
  int w = 0, h = 0;
  int bw = 0, bh = 0;           // blocks across and down
  std::vector<uint8_t> luma;    // w * h, a luma code (0..14) a pixel; 15 outside
  std::vector<uint8_t> paint;   // the silhouette, from the luma plane: rows of (w + 7) / 8 bytes
  std::vector<int8_t> cb, cr;   // bw * bh, the block's B - Y and R - Y
  uint8_t lumaTable[16] = {0};  // luma code -> Y
  // Colour a pixel, RGB565, when the sprite was decoded some other way than
  // the posterised planes - the host harness's codec experiments. Empty for a
  // sprite from the pack; `luma` and `cb`/`cr` are then unused, `paint` still
  // the silhouette.
  std::vector<uint16_t> direct;

  inline bool painted(int x, int y) const {
    return paint[size_t(y) * ((w + 7) / 8) + (x >> 3)] & (0x80 >> (x & 7));
  }
  // The pixel's colour, RGB565. Fixed point: a pixel sits at (x + 0.5) / 4
  // in block units, block centres at n + 0.5, so its position among the
  // centres is (x - 1.5) / 4 - in 1/64ths, 16 * x - 24 - split into a block
  // index and a 6-bit fraction, clamped at the edges.
  inline uint16_t at(int x, int y) const {
    if (!direct.empty()) return direct[size_t(y) * w + x];
    const int Y = lumaTable[luma[size_t(y) * w + x] & 15];
    int tx = 16 * x - 24, ty = 16 * y - 24;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    int bx = tx >> 6, by = ty >> 6;
    int fx = tx & 63, fy = ty & 63;
    if (bx >= bw - 1) bx = bw - 1, fx = 0;
    if (by >= bh - 1) by = bh - 1, fy = 0;
    const size_t i00 = size_t(by) * bw + bx;
    const size_t i01 = i00 + (fx ? 1 : 0), i10 = i00 + (fy ? bw : 0), i11 = i10 + (fx ? 1 : 0);
    const int w00 = (64 - fx) * (64 - fy), w01 = fx * (64 - fy), w10 = (64 - fx) * fy, w11 = fx * fy;
    const int cbv = (cb[i00] * w00 + cb[i01] * w01 + cb[i10] * w10 + cb[i11] * w11) >> 12;
    const int crv = (cr[i00] * w00 + cr[i01] * w01 + cr[i10] * w10 + cr[i11] * w11) >> 12;
    // Y + Cr is red, Y + Cb is blue, and green is what is left of the luma.
    const int r = Y + crv, b = Y + cbv;
    const int g = (Y * 256 - 77 * r - 28 * b) / 151;
    return rgb565c(r, g, b);
  }

 private:
  static inline uint16_t rgb565c(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
};

class Plates {
 public:
  // Parse the header and index. Returns false, with `error` set, if the file
  // is not a v6 pack.
  bool open(Reader reader, std::string *error = nullptr);

  size_t count() const { return entries_.size(); }
  const PlateEntry &entry(size_t i) const { return entries_[i]; }
  // Exact match on the scientific name, or -1. The bake keys on the same
  // normalised names the desktop renderer does, so no aliasing happens here.
  int find(const std::string &scientific) const;

  const uint8_t *paper() const { return paper_; }  // rgb
  int source() const { return source_; }

  // Inflate the silhouette into a packer mask (w x h).
  bool loadMask(size_t i, Mask &out) const;
  // Inflate the planes and expand them to codes and a colour table.
  bool loadSprite(size_t i, SpriteImage &out) const;

  // One sprite on its own, as `tools/export_web_plates.py` writes it for the
  // frame to fetch over the network: the same planes and tables as a pack
  // record, behind a small header ('FGPS' v1). The full-size supplement to
  // the pack in flash; see App::showBirds.
  static bool decodeSingle(const std::string &file, SpriteImage &out, std::string *error = nullptr);

 private:
  // The planes of `e`, read through `reader` from `base`, expanded into `out`.
  static bool decode(const PlateEntry &e, const Reader &reader, uint32_t base, SpriteImage &out);

  Reader reader_;
  std::vector<PlateEntry> entries_;
  uint8_t paper_[3] = {0, 0, 0};
  int source_ = 0;
  uint32_t payload_ = 0;  // file offset the record offsets are relative to
};

}  // namespace birdposter
