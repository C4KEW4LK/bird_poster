// Putting pixels on the page: composite, dither, text and QR codes.
//
// Two rasters, and the split is the design. The Canvas is one byte a pixel of
// indices into the pack's 256-colour palette - what the page would look like
// if the panel could show it. The Frame is one byte a pixel of the six inks the
// panel actually has. A page is composed on the canvas and dithered to the
// frame in one pass, because error diffusion has to see the whole page: a
// sprite dithered on its own would carry a seam of its own pattern.
//
// Text goes straight onto the frame, after the dither, as solid black. A name
// dithered from grey antialiasing turns into colour speckle; the reference
// renderer hard-thresholds labels for the same reason.
//
// Nothing here touches Arduino. The host harness renders the same page to a
// file, which is how this was checked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "plates.h"

namespace birdposter {

// The panel's six inks, in the driver's own order.
enum Ink : uint8_t { kBlack = 0, kWhite = 1, kYellow = 2, kRed = 3, kBlue = 4, kGreen = 5 };

// What each ink looks like, for the dither to measure distance against. Half
// way between the pure primaries and the panel's measured pigments
// (dither.py's SATURATION = 0.5), so distances are taken in roughly panel space.
extern const uint8_t kInkRgb[6][3];

// The paper tone the reference page is set on (paper.TARGET_PAPER).
constexpr uint8_t kPaperRgb[3] = {242, 237, 226};

// The inks as the dither measures against them: kInkRgb scaled so the
// panel's white lands on the paper. The panel's white is a grey (208) and the
// page's paper is not (242); measured raw, everything in a plate lighter than
// 208 - a white bird's shading, its grey outline, a pale wash - is "brighter
// than white", clamps to pure white, and the bird disappears into the page.
// The eye takes the panel's white for white, so the paper is the white point
// and the inks are placed relative to it. The preview keeps kInkRgb: that is
// what the glass looks like.
extern const uint8_t kInkTarget[6][3];

// RGB565 helpers. The canvas is 16 bits a pixel: enough for a 256-colour
// sprite's table to land losslessly, and half what 24 bits would cost in
// PSRAM at 1600 x 1200.
inline uint16_t rgb565(int r, int g, int b) {
  return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void rgb888(uint16_t c, int &r, int &g, int &b) {
  r = ((c >> 11) & 31) * 255 / 31;
  g = ((c >> 5) & 63) * 255 / 63;
  b = (c & 31) * 255 / 31;
}

struct Canvas {
  int w = 0, h = 0;
  std::vector<uint16_t> px;  // RGB565
  void reset(int width, int height, uint16_t fill);
  inline uint16_t *row(int y) { return px.data() + size_t(y) * w; }
  inline const uint16_t *row(int y) const { return px.data() + size_t(y) * w; }
};

struct Frame {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // Ink values
  void reset(int width, int height, Ink fill);
  inline uint8_t *row(int y) { return px.data() + size_t(y) * w; }
  inline const uint8_t *row(int y) const { return px.data() + size_t(y) * w; }
  inline void set(int x, int y, Ink ink) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[size_t(y) * w + x] = ink;
  }
  void fillRect(int x, int y, int rw, int rh, Ink ink);
};

// How a sprite drawn larger than it was baked - or shrunk by under 2x - is
// filled in between its pixels.
//   Bilinear    the four around; smooth but soft
//   Mitchell    bicubic, B = C = 1/3: sharper, with almost no halo beside a
//               dark line - which the edge filter would otherwise ink
//   CatmullRom  bicubic, B = 0, C = 1/2: sharper again, with a faint halo
enum class Resample : uint8_t { Bilinear = 0, Mitchell = 1, CatmullRom = 2 };

// Draw a sprite scaled to `dstW` x `dstH` with its top-left at (x, y),
// optionally mirrored. Clear pixels are skipped, so the halo of one bird does
// not paint over the body of another - the packer only guarantees that
// silhouettes do not overlap, not that their padding does not. Growing, or
// shrinking by under 2x, is filtered by `resample`; shrinking further is a
// box filter.
void drawSprite(Canvas &canvas, const SpriteImage &sprite, int x, int y, int dstW, int dstH,
                bool flip, Resample resample = Resample::Bilinear);

// How hard to push the source's colour before it meets the six inks. The
// panel's gamut is small and its white is grey, and a faithful reduction of a
// soft watercolour plate reads as washed out on it; stretching saturation and
// contrast first spends more coloured ink per area, which is what "vivid"
// looks like on this glass. Applied to the canvas palette, so it costs 256
// entries, not a page of pixels. 0 is as printed; the levels are in the .cpp.
constexpr int kVividLevels = 5;

// Error diffusion is a low-pass filter: a one-pixel line against paper comes
// out as a few scattered dots, and a plate's fine hatching goes with it. An
// unsharp mask ahead of the dither puts the local contrast back so the
// diffusion has something to land ink on. 0 is off; levels in the .cpp.
constexpr int kSharpenLevels = 5;

// Sharpening amplifies the contrast an edge already has, so a faint one stays
// faint. An edge *detector* (Sobel) finds the boundary whatever its contrast,
// and ink is laid along it on a curve that lifts the faint ones: a white
// bird's outline against the paper becomes a drawn line, as it was on the
// lithograph before the scan softened it. 0 is off; levels in the .cpp.
constexpr int kEdgeLevels = 5;

// Floyd-Steinberg, serpentine, canvas to the six inks, with the colour
// boost, the sharpening and the edge ink applied on the way in. The boost is
// a 64K-entry table over RGB565, built per call, so it is a lookup a pixel.
//
// `jitter` breaks up the patterns error diffusion draws through flat tone -
// the near-regular lattices of dots on a pale background - by nudging each
// pixel's value by up to that much, repeatably, before the ink is chosen.
// The error passed on is still taken from the true value, so the average
// colour holds; only where the dots fall is randomised. 0 is off.
void dither(const Canvas &canvas, Frame &out, int vivid = 0, int sharpen = 0, int edges = 0,
            int jitter = 0);

// --- text -------------------------------------------------------------------

// A TrueType face, kept as the file's bytes. `stb_truetype` reads glyphs out of
// it on demand, so a whole family costs the file and nothing more.
class Font {
 public:
  bool load(std::vector<uint8_t> bytes);
  bool ok() const { return !info_.empty(); }

  // Width in pixels of `text` set at `px` em size - the same size PIL means by
  // `ImageFont.truetype(path, px)`, so a label box measured there is measured
  // here. Also returns the line's ascent and descent so a caller can stack lines.
  int measure(const std::string &text, int px, int *ascent = nullptr,
              int *descent = nullptr) const;

  // Draw `text` with its left edge at x and baseline at y, solid `ink`. Glyph
  // coverage is thresholded at half: no antialiasing survives on the panel
  // anyway, and grey would dither into speckle.
  void draw(Frame &frame, const std::string &text, int x, int baseline, int px, Ink ink) const;

  // Whether the face has a glyph for every character of `text`. A subset
  // face - the common name's is capitals only - is asked before it is used,
  // so a letter it lacks falls back to another face rather than a box.
  bool covers(const std::string &text) const;

  // Keep hairlines: dropout control, as a hinting rasteriser does it. A face
  // like Gould's has strokes about a fiftieth of the size thick - under half
  // a pixel at label sizes - and the threshold above drops them: an H loses
  // its bar, or a thin stem. With this on, a pixel under the threshold is
  // still inked where it is the most covered across a thin line (left to
  // right, or top to bottom) and neither neighbour on that line is inked, so
  // a line that would vanish lands exactly one pixel wide. Strokes that
  // already land, and the gaps between them, are drawn as before.
  void setKeepHairlines(bool on) { keepHairlines_ = on; }

 private:
  bool keepHairlines_ = false;
  std::vector<uint8_t> bytes_;
  std::vector<uint8_t> info_;  // an opaque stbtt_fontinfo
};

// Draw `text` centred in the box - what the reference does with a label: it
// reserved a box wide enough at the measured size and centres the redrawn
// name inside it, because a re-rasterised face is not exactly width x scale.
void drawCentred(Frame &frame, const Font &font, const std::string &text, int boxX, int boxY,
                 int boxW, int boxH, int px, Ink ink);

// --- QR ---------------------------------------------------------------------

// Encode `text` and draw it with `module` pixels a module, top-left at (x, y),
// with the quiet zone the standard asks for. Returns the drawn size in pixels,
// or 0 if the text does not fit a QR code.
int drawQr(Frame &frame, const std::string &text, int x, int y, int module);

// Side, in pixels, that drawQr would draw for this text at this module size.
int qrSize(const std::string &text, int module);

// The string a phone's camera turns into a "join this network" prompt.
// Android and iOS both read it; `pass` empty means an open network.
std::string wifiQrPayload(const std::string &ssid, const std::string &pass);

}  // namespace birdposter
