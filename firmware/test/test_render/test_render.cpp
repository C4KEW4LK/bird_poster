// The renderer and the pack reader, on the host. What these pin is the part
// the panel cannot argue with: the dither lands solid colours on their own
// ink and paper on white, a sprite's clear pixels leave the page alone, the
// WiFi QR payload is escaped the way a phone expects, and a pack that is not
// a pack is refused rather than read.
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pages.h"
#include "planecoder.h"
#include "plates.h"
#include "render.h"

using namespace birdposter;

namespace {

// The inks in the source's space - what a plate would have to be painted to
// come out as that ink - which is kInkTarget, not the panel's own kInkRgb.
uint16_t inkColour(int ink) {
  return rgb565(kInkTarget[ink][0], kInkTarget[ink][1], kInkTarget[ink][2]);
}
uint16_t paperColour() { return rgb565(kPaperRgb[0], kPaperRgb[1], kPaperRgb[2]); }
// A "grey" here is the paper scaled toward black, as a wash on a plate is:
// a neutral grey is cool against warm paper and dithers to blue, which is a
// different matter from the one under test.
uint16_t wash(float k) {
  return rgb565(int(kPaperRgb[0] * k + 0.5f), int(kPaperRgb[1] * k + 0.5f),
                int(kPaperRgb[2] * k + 0.5f));
}
int count(const Frame &f, Ink ink, int y0 = 0, int y1 = 1 << 30) {
  int n = 0;
  for (int y = std::max(0, y0); y < std::min(f.h, y1); ++y)
    for (int x = 0; x < f.w; ++x) n += f.row(y)[x] == ink;
  return n;
}
void fillRows(Canvas &c, int y0, int y1, uint16_t v) {
  for (int y = y0; y < y1; ++y) std::fill(c.row(y), c.row(y) + c.w, v);
}

void test_each_ink_dithers_to_itself() {
  // RGB565 rounds the target by a level or so, and a level of error per pixel
  // is a stray dot every few hundred: 99% is what "itself" means here.
  for (int ink = 0; ink < 6; ++ink) {
    Canvas c;
    c.reset(64, 16, inkColour(ink));
    Frame f;
    dither(c, f);
    TEST_ASSERT_TRUE(count(f, Ink(ink)) >= 64 * 16 * 99 / 100);
  }
}

void test_dithering_in_place_matches_a_separate_frame() {
  // The frame written over the canvas it reads (render.h): byte for byte
  // the frame a separate buffer gets, with every option on, so a row read
  // after its memory was written would show.
  constexpr int w = 97, h = 61;
  Canvas c;
  c.reset(w, h, paperColour());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      c.row(y)[x] = uint16_t((x * 2654435761u) ^ (y * 40503u) ^ (x * y * 97u));
  Frame apart;
  dither(c, apart, 2, 2, 2, 24);

  std::vector<uint16_t> block(size_t(w) * h);
  Canvas shared;
  shared.px.adopt(block.data(), block.size());
  shared.reset(w, h, 0);
  std::copy(c.px.begin(), c.px.end(), shared.px.begin());
  Frame inPlace;
  inPlace.w = w;
  inPlace.h = h;
  inPlace.px.adopt(reinterpret_cast<uint8_t *>(block.data()), block.size() * 2, size_t(w) * h);
  dither(shared, inPlace, 2, 2, 2, 24);
  TEST_ASSERT_EQUAL_PTR(block.data(), inPlace.px.data());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(apart.px.data(), inPlace.px.data(), size_t(w) * h);
}

void test_paper_dithers_to_white_not_speckle() {
  // Paper is brighter than the panel's white, and a dither that let that error
  // pile up would eventually flip a pixel to yellow. It must not.
  Canvas c;
  c.reset(400, 300, paperColour());
  Frame f;
  dither(c, f);
  for (uint8_t px : f.px) TEST_ASSERT_EQUAL_UINT8(kWhite, px);
}

// A sprite built by hand: a colour a pixel (the `direct` form the decoders
// can fill) and a paint bit a pixel.
SpriteImage sprite(int w, int h, std::vector<uint16_t> colours, std::vector<uint8_t> paintBits) {
  SpriteImage s;
  s.w = w;
  s.h = h;
  s.direct = std::move(colours);
  s.paint = std::move(paintBits);
  return s;
}

void test_a_sprites_clear_pixels_do_not_paint() {
  // A 4x2 sprite: left half painted code 0, right half not painted.
  const uint16_t ink = rgb565(200, 40, 40);
  SpriteImage s = sprite(4, 2, std::vector<uint16_t>(8, ink), {0xC0, 0xC0});
  Canvas c;
  c.reset(8, 4, 0xFFFF);
  drawSprite(c, s, 2, 1, 4, 2, false);
  TEST_ASSERT_EQUAL_UINT16(ink, c.row(1)[2]);
  TEST_ASSERT_EQUAL_UINT16(ink, c.row(1)[3]);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, c.row(1)[4]);  // clear: paper survives
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, c.row(1)[5]);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, c.row(0)[2]);  // above the sprite
  // Mirrored, the painted half is on the right.
  c.reset(8, 4, 0xFFFF);
  drawSprite(c, s, 2, 1, 4, 2, true);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, c.row(1)[2]);
  TEST_ASSERT_EQUAL_UINT16(ink, c.row(1)[5]);
}

void test_a_grown_sprite_blends_between_its_pixels() {
  // Two pixels grown to eight: the ends keep their own colours, and in
  // between the colour runs from one to the other rather than stepping.
  SpriteImage s = sprite(2, 1, {rgb565(0, 0, 0), rgb565(248, 248, 248)}, {0xC0});
  for (Resample r : {Resample::Bilinear, Resample::Mitchell, Resample::CatmullRom}) {
    Canvas c;
    c.reset(8, 1, 0x1234);
    drawSprite(c, s, 0, 0, 8, 1, false, r);
    // The ends are their own pixels' colours - to within a step: the cubics
    // sharpen by overshooting a little at an edge, which is what they are for.
    int er, eg, eb;
    rgb888(c.row(0)[0], er, eg, eb);
    TEST_ASSERT_TRUE(er <= 8 && eg <= 8 && eb <= 8);
    rgb888(c.row(0)[7], er, eg, eb);
    TEST_ASSERT_TRUE(er >= 240 && eg >= 240 && eb >= 240);
    int prev = -1;
    for (int x = 0; x < 8; ++x) {
      int red, green, blue;
      rgb888(c.row(0)[x], red, green, blue);
      TEST_ASSERT_TRUE(red >= prev);  // never darker going right
      prev = red;
    }
    int mr, mg, mb;
    rgb888(c.row(0)[4], mr, mg, mb);
    TEST_ASSERT_TRUE(mr > 0 && mr < 248);  // a blend, not either end
  }
}


void test_wifi_qr_payload_escapes_what_the_format_reserves() {
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:birdposter-3A7F;P:birdposter;;",
                           wifiQrPayload("birdposter-3A7F", "birdposter").c_str());
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\;b\\:c;P:x\\,y\\\\z\\\";;",
                           wifiQrPayload("a;b:c", "x,y\\z\"").c_str());
  TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:open;;", wifiQrPayload("open", "").c_str());
}

void test_a_qr_code_is_drawn_with_its_quiet_zone() {
  Frame f;
  f.reset(400, 400, kRed);
  const int side = drawQr(f, "http://192.168.4.1/", 10, 10, 4);
  TEST_ASSERT_TRUE(side > 0);
  TEST_ASSERT_EQUAL_INT(side, qrSize("http://192.168.4.1/", 4));
  // Quiet zone: the first four modules in from the edge are white.
  for (int i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_UINT8(kWhite, f.row(10 + i)[10 + i]);
  // The finder pattern's outer ring starts right after it.
  TEST_ASSERT_EQUAL_UINT8(kBlack, f.row(10 + 16)[10 + 16]);
  // Outside the code the frame is untouched.
  TEST_ASSERT_EQUAL_UINT8(kRed, f.row(10 + side)[10 + side]);
}

// The vivid boost stretches colour, not the page: paper stays white and each
// ink stays on its own ink at the top level, while a pale tint spends more
// coloured ink than a faithful reduction would.
void test_vivid_spends_more_ink_on_a_tint_and_none_on_the_paper() {
  Canvas c;
  c.reset(64, 64, rgb565(240, 230, 150));  // a pale yellow wash
  Frame plain, vivid;
  dither(c, plain, 0);
  dither(c, vivid, kVividLevels - 1);
  TEST_ASSERT_TRUE(count(plain, kYellow) > 0);
  TEST_ASSERT_TRUE(count(vivid, kYellow) > count(plain, kYellow));

  c.reset(64, 64, paperColour());
  dither(c, vivid, kVividLevels - 1);
  TEST_ASSERT_EQUAL_INT(64 * 64, count(vivid, kWhite));
  for (int ink = 0; ink < 6; ++ink) {
    c.reset(16, 16, inkColour(ink));
    dither(c, vivid, kVividLevels - 1);
    TEST_ASSERT_TRUE(count(vivid, Ink(ink)) >= 16 * 16 * 97 / 100);
  }
}

// Error diffusion is also a low-pass filter: a one-pixel mid line against
// paper comes out as scattered dots and the line breaks. Sharpened, more of
// the line lands as ink, and the paper either side stays paper.
void test_sharpening_keeps_a_thin_line_and_leaves_the_paper_alone() {
  Canvas c;
  c.reset(64, 64, paperColour());
  fillRows(c, 32, 33, wash(0.5f));
  Frame soft, sharp;
  dither(c, soft, 0, 0);
  dither(c, sharp, 0, kSharpenLevels - 1);
  TEST_ASSERT_TRUE(count(sharp, kBlack, 32, 33) > count(soft, kBlack, 32, 33));
  int offLine = 0;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
      if (y < 30 || y > 34) offLine += sharp.row(y)[x] != kWhite;
  TEST_ASSERT_EQUAL_INT(0, offLine);
}

// Edge ink is laid along a boundary and nowhere else: a faint step between
// two light washes gets a line it would not otherwise have, and the flat
// fields either side stay as they were.
void test_edge_ink_draws_a_faint_boundary_and_nothing_else() {
  Canvas c;
  c.reset(64, 64, wash(0.97f));
  fillRows(c, 32, 64, wash(0.90f));
  Frame plain, edged;
  dither(c, plain, 0, 0, 0);
  dither(c, edged, 0, 0, kEdgeLevels - 1);
  TEST_ASSERT_TRUE(count(edged, kBlack, 30, 34) > count(plain, kBlack, 30, 34) + 15);
  TEST_ASSERT_TRUE(std::abs(count(edged, kBlack, 0, 24) - count(plain, kBlack, 0, 24)) < 8);
  TEST_ASSERT_TRUE(std::abs(count(edged, kBlack, 40, 64) - count(plain, kBlack, 40, 64)) < 8);
}

// The ink goes to the light: the same step between two dark washes earns a
// fraction of the line the light pair does.
void test_edge_ink_favours_light_over_dark() {
  auto lineInk = [](uint16_t a, uint16_t b) {
    Canvas c;
    c.reset(64, 64, a);
    fillRows(c, 32, 64, b);
    Frame plain, edged;
    dither(c, plain, 0, 0, 0);
    dither(c, edged, 0, 0, kEdgeLevels - 1);
    return count(edged, kBlack, 30, 34) - count(plain, kBlack, 30, 34);
  };
  const int light = lineInk(wash(0.97f), wash(0.90f)), dark = lineInk(wash(0.32f), wash(0.25f));
  TEST_ASSERT_TRUE(light > 15);
  TEST_ASSERT_TRUE(dark * 2 < light);
}


bool loadFont(Font &font) {
  std::ifstream in("../assets/fonts/gentiumbookplus/GentiumBookPlus-Italic.ttf", std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return font.load(std::move(bytes));
}

// A common name goes over the scientific one, so the box the packer reserves
// has to hold both lines: taller than either, wide as the wider.
void test_a_name_pair_reserves_room_for_both_lines() {
  Font font;
  TEST_ASSERT_TRUE_MESSAGE(loadFont(font), "run from firmware/: needs ../assets/fonts");
  const int px = 100;
  int a = 0, d = 0;
  const int common = font.measure("Maned Duck", px, &a, &d);
  const int oneLine = a + d;
  const int sci = font.measure("Chenonetta jubata", int(px * kSubNameScale), &a, &d);
  const LabelBox box = nameBox(font, font, "Maned Duck", "Chenonetta jubata", px);
  TEST_ASSERT_EQUAL_INT(std::max(common, sci), box.w);
  TEST_ASSERT_TRUE(box.h > oneLine);
  TEST_ASSERT_TRUE(box.h >= oneLine + a + d);

  // Drawn, both lines put ink inside the box and nothing outside it.
  Frame f;
  f.reset(box.w + 40, box.h + 40, kWhite);
  drawName(f, font, font, "Maned Duck", "Chenonetta jubata", 20, 20, box.w, box.h, px);
  int inkTop = 0, inkBottom = 0, outside = 0;
  for (int y = 0; y < f.h; ++y)
    for (int x = 0; x < f.w; ++x) {
      if (f.row(y)[x] != kBlack) continue;
      const bool in = x >= 20 && x < 20 + box.w && y >= 20 && y < 20 + box.h;
      if (!in) ++outside;
      else if (y < 20 + box.h / 2) ++inkTop;
      else ++inkBottom;
    }
  TEST_ASSERT_EQUAL_INT(0, outside);
  TEST_ASSERT_TRUE(inkTop > 0);
  TEST_ASSERT_TRUE(inkBottom > 0);
  // The lower line is the smaller face, so it carries less ink.
  TEST_ASSERT_TRUE(inkBottom < inkTop);
}

// A v7 pack built by hand, byte by byte, and read back: the format is the
// interface between the bake and the frame, and this is where it is pinned.
namespace {
void put16(std::vector<uint8_t> &b, uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void put32(std::vector<uint8_t> &b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

// The plane the Python coder was run on to make the bytes below: whatever
// either side changes, the two must still agree to the byte.
constexpr int kFixW = 12, kFixH = 9;
std::vector<uint8_t> fixtureLuma() {
  std::vector<uint8_t> l;
  for (int y = 0; y < kFixH; ++y)
    for (int x = 0; x < kFixW; ++x) l.push_back((x + y) % 7 == 0 ? 15 : uint8_t((x * 3 + y * 5) % 15));
  return l;
}
std::vector<uint8_t> fixtureChroma() {
  std::vector<uint8_t> c;
  for (int by = 0; by < 5; ++by)
    for (int bx = 0; bx < 6; ++bx) c.push_back(uint8_t((bx * 7 + by * 3) % 16));
  return c;
}
// planecoder.encode_luma / encode_chroma(block 2) of the planes above. The
// luma bytes depend on plane_prior.h: retrain it and these are regenerated.
const std::vector<uint8_t> kPyLuma{
    0x04, 0x84, 0xAB, 0xBB, 0x04, 0x2A, 0x7E, 0xE7, 0x18, 0xF7, 0x75, 0x50, 0x7D, 0xC8, 0x41,
    0xB8, 0xD2, 0x43, 0x97, 0x8E, 0x3E, 0xE0, 0xA5, 0x63, 0x9B, 0x43, 0xA0, 0xA4, 0xDD, 0x1F,
    0x63, 0x4D, 0x55, 0x9C, 0x2C, 0x23, 0x6B, 0xDE, 0x61, 0xD4, 0xCF, 0x31, 0x6A, 0xBA, 0x57,
    0x3B, 0x8B, 0xE4, 0x9B, 0x4C, 0xC4, 0xA1, 0xBD, 0x05, 0xE1, 0xB8, 0x97, 0xE0, 0x45, 0xF3,
    0x58, 0x5E, 0x5D, 0x09, 0xEB, 0x9A, 0x42, 0xD8, 0x2B, 0x85, 0x00, 0xF2, 0xAB, 0x09, 0x9C,
    0xE2, 0x60, 0xDB, 0xF1, 0xCB, 0xE0, 0x36, 0xF3};
const std::vector<uint8_t> kPyChroma{0x07, 0xE5, 0xC3, 0x32, 0x33, 0x33, 0x33, 0x32, 0x33,
                                     0x23, 0x33, 0x33, 0x33, 0x26, 0x61, 0xA2, 0xAF};
}  // namespace

void test_the_plane_coder_agrees_with_the_bake_to_the_byte() {
  const std::vector<uint8_t> luma = fixtureLuma(), chroma = fixtureChroma();
  TEST_ASSERT_TRUE(planecoder::encodeLuma(luma.data(), kFixW, kFixH) == kPyLuma);
  TEST_ASSERT_TRUE(planecoder::encodeChroma(chroma.data(), luma.data(), kFixW, kFixH, 2) == kPyChroma);

  std::vector<uint8_t> l(luma.size()), c(chroma.size());
  TEST_ASSERT_TRUE(planecoder::decodeLuma(kPyLuma.data(), kPyLuma.size(), kFixW, kFixH, l.data()));
  TEST_ASSERT_TRUE(l == luma);
  TEST_ASSERT_TRUE(planecoder::decodeChroma(kPyChroma.data(), kPyChroma.size(), l.data(), kFixW,
                                            kFixH, 2, c.data()));
  // Every block here has something painted in it, so every code comes back.
  TEST_ASSERT_TRUE(c == chroma);
}

void test_the_plane_coder_round_trips_and_refuses_a_cut_stream() {
  // A bird-like plane: a disc of smooth shading with noise, outside around it.
  constexpr int w = 97, h = 61;
  std::vector<uint8_t> luma(size_t(w) * h), chroma(size_t((w + 1) / 2) * ((h + 1) / 2));
  uint32_t seed = 12345;
  const auto rnd = [&seed] { return (seed = seed * 1103515245u + 12345u) >> 16; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int dx = x - w / 2, dy = y - h / 2;
      luma[size_t(y) * w + x] =
          dx * dx + dy * dy > 28 * 28 ? 15 : uint8_t(std::min(14, int((x + y) / 12 + rnd() % 3)));
    }
  for (auto &c : chroma) c = uint8_t(rnd() % 4 == 0 ? rnd() % 16 : 3);

  const std::vector<uint8_t> zl = planecoder::encodeLuma(luma.data(), w, h);
  const std::vector<uint8_t> zc = planecoder::encodeChroma(chroma.data(), luma.data(), w, h, 2);
  std::vector<uint8_t> l(luma.size()), c(chroma.size());
  TEST_ASSERT_TRUE(planecoder::decodeLuma(zl.data(), zl.size(), w, h, l.data()));
  TEST_ASSERT_TRUE(l == luma);
  TEST_ASSERT_TRUE(planecoder::decodeChroma(zc.data(), zc.size(), l.data(), w, h, 2, c.data()));
  // The painted blocks come back; the rest are 0 and never stored.
  for (int by = 0; by < (h + 1) / 2; ++by)
    for (int bx = 0; bx < (w + 1) / 2; ++bx) {
      bool painted = false;
      for (int y = by * 2; y < std::min(h, by * 2 + 2); ++y)
        for (int x = bx * 2; x < std::min(w, bx * 2 + 2); ++x) painted |= luma[size_t(y) * w + x] != 15;
      const size_t i = size_t(by) * ((w + 1) / 2) + bx;
      TEST_ASSERT_EQUAL_UINT8(painted ? chroma[i] : 0, c[i]);
    }
  // A stream one byte short, or one byte long, is not this plane.
  TEST_ASSERT_FALSE(planecoder::decodeLuma(zl.data(), zl.size() - 1, w, h, l.data()));
  std::vector<uint8_t> longer = zl;
  longer.push_back(0);
  TEST_ASSERT_FALSE(planecoder::decodeLuma(longer.data(), longer.size(), w, h, l.data()));
}

// A whole v7 pack of one bird, built byte by byte. Shared with the margin
// test below, which wants a pack to render rather than a format to check.
std::vector<uint8_t> v7FixturePack() {
  // An 8x4 sprite, four by two chroma blocks. Inside the silhouette: row 0
  // x0-3, row 1 x4-7, row 2 all, row 3 none; 15 outside, else the pixel's
  // index inside (0-14, so the last inside pixel wraps to 0).
  std::vector<uint8_t> luma;
  int inside = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x) {
      const bool in = (y == 0 && x < 4) || (y == 1 && x >= 4) || y == 2;
      luma.push_back(in ? uint8_t(inside++ % 15) : 15);
    }
  // The right-hand column of blocks is code 1, the rest code 0.
  std::vector<uint8_t> chroma(8, 0);
  chroma[3] = chroma[7] = 1;
  const std::vector<uint8_t> zl = planecoder::encodeLuma(luma.data(), 8, 4);
  const std::vector<uint8_t> zc = planecoder::encodeChroma(chroma.data(), luma.data(), 8, 4, 2);

  std::vector<uint8_t> pack{'F', 'G', 'P', 'L'};
  put32(pack, 7);
  pack.push_back(4);  // depth
  pack.push_back(2);  // chroma block
  put16(pack, 8);     // source
  put32(pack, 1);     // count
  pack.push_back(242); pack.push_back(237); pack.push_back(226);
  put16(pack, 2); pack.push_back('A'); pack.push_back('a');
  put16(pack, 8); put16(pack, 4);
  pack.push_back(1); pack.push_back(0);
  put16(pack, 10); put16(pack, 5);
  for (int i = 0; i < 16; ++i) pack.push_back(uint8_t(i * 16));   // luma table
  for (int i = 0; i < 16; ++i) pack.push_back(uint8_t(i == 1 ? 50 : 0));     // cb
  for (int i = 0; i < 16; ++i) pack.push_back(uint8_t(i == 1 ? int8_t(-50) : 0));  // cr
  put32(pack, 0); put32(pack, uint32_t(zl.size())); put32(pack, uint32_t(zc.size()));
  for (const auto *z : {&zl, &zc}) pack.insert(pack.end(), z->begin(), z->end());
  return pack;
}

// Read `pack` the way the frame reads its flash.
bool openPack(const std::vector<uint8_t> &pack, Plates &p, std::string *err) {
  return p.open(
      [&pack](uint32_t offset, void *dst, size_t len) {
        if (size_t(offset) + len > pack.size()) return false;
        std::memcpy(dst, pack.data() + offset, len);
        return true;
      },
      err);
}

void test_a_v7_pack_round_trips_through_the_reader() {
  const std::vector<uint8_t> pack = v7FixturePack();
  Plates p;
  std::string err;
  const bool ok = openPack(pack, p, &err);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL_size_t(1, p.count());
  TEST_ASSERT_EQUAL_INT(0, p.find("Aa"));
  const PlateEntry &e = p.entry(0);
  TEST_ASSERT_EQUAL_INT(8, e.w);
  TEST_ASSERT_EQUAL_INT(4, e.h);
  TEST_ASSERT_TRUE(e.flip);
  TEST_ASSERT_EQUAL_INT(10, e.label.w);
  TEST_ASSERT_EQUAL_UINT8(242, p.paper()[0]);

  SpriteImage s;
  TEST_ASSERT_TRUE(p.loadSprite(0, s));
  TEST_ASSERT_TRUE(s.painted(0, 0));
  TEST_ASSERT_FALSE(s.painted(4, 0));
  TEST_ASSERT_FALSE(s.painted(0, 3));
  // (0,0): painted index 0, block 0 -> code 0 -> Y 0, no chroma -> black.
  TEST_ASSERT_EQUAL_UINT16(rgb565(0, 0, 0), s.at(0, 0));
  // (7,1): painted index 7 -> Y 112. Past the last block centre across, and
  // between two code-1 blocks down, so the chroma is code 1's alone, not a
  // blend: cb +50, cr -50.
  const int y = 112, r = y - 50, b = y + 50;
  const int g = int(std::lround((y - 0.299 * r - 0.114 * b) / 0.587));
  TEST_ASSERT_EQUAL_UINT16(rgb565(r, g, b), s.at(7, 1));
  // (7,2): inside index 15 wraps to code 0.
  TEST_ASSERT_EQUAL_UINT8(0, s.luma[2 * 8 + 7]);

  // The packer's mask is the same bits.
  Mask m;
  TEST_ASSERT_TRUE(p.loadMask(0, m));
  TEST_ASSERT_EQUAL_INT(8, m.width());
  TEST_ASSERT_TRUE(m.get(0, 0));
  TEST_ASSERT_FALSE(m.get(4, 0));
  TEST_ASSERT_TRUE(m.get(4, 1));
  TEST_ASSERT_TRUE(m.get(7, 2));
  TEST_ASSERT_FALSE(m.get(0, 3));
}

// The owner's margin is a border the page draws nothing in, a side at a time,
// so a mount over the glass cannot cut a name off the edge. The birds are
// packed into what is left, so the band around them stays paper - and paper
// dithers to white, which is what makes this checkable.
void test_a_page_margin_leaves_its_border_untouched() {
  const std::vector<uint8_t> pack = v7FixturePack();
  Plates p;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(openPack(pack, p, &err), err.c_str());

  const Font noFace;  // nothing loaded: the names and the date draw nothing
  // The box every pixel the page drew falls inside.
  const auto inkBox = [&](const BirdPageSettings &settings, int box[4]) {
    Frame out;
    TEST_ASSERT_TRUE(renderBirdPage(p, {0}, settings, noFace, out));
    box[0] = out.w, box[1] = out.h, box[2] = -1, box[3] = -1;
    for (int y = 0; y < out.h; ++y)
      for (int x = 0; x < out.w; ++x) {
        if (out.row(y)[x] == kWhite) continue;
        box[0] = std::min(box[0], x), box[1] = std::min(box[1], y);
        box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
      }
    TEST_ASSERT_TRUE_MESSAGE(box[2] >= 0, "nothing was drawn at all");
  };

  BirdPageSettings s;
  int bare[4];
  inkBox(s, bare);
  s.marginTop = 120;
  s.marginRight = 40;
  s.marginBottom = 200;
  s.marginLeft = 80;
  int inset[4];
  inkBox(s, inset);

  const int width = bare[2] - bare[0] + 1, height = bare[3] - bare[1] + 1;
  TEST_ASSERT_TRUE_MESSAGE(inset[0] >= s.marginLeft && inset[1] >= s.marginTop &&
                               inset[2] < 1600 - s.marginRight && inset[3] < 1200 - s.marginBottom,
                           "the page drew in its margin");
  // One bird is sized off the page rather than off the room left for it, so
  // it comes out the same size either way - the margin moves it, by half of
  // what it took off each pair of sides, and nothing else.
  TEST_ASSERT_INT_WITHIN(2, width, inset[2] - inset[0] + 1);
  TEST_ASSERT_INT_WITHIN(2, height, inset[3] - inset[1] + 1);
  TEST_ASSERT_INT_WITHIN(2, bare[0] + (s.marginLeft - s.marginRight) / 2, inset[0]);
  TEST_ASSERT_INT_WITHIN(2, bare[1] + (s.marginTop - s.marginBottom) / 2, inset[1]);
}

// The owner's lines: names filled in from the clock, the rest left alone,
// and nothing at all from a clock that is not set.
void test_page_text_fills_in_the_clock() {
  std::tm tm{};
  tm.tm_year = 2026 - 1900, tm.tm_mon = 8, tm.tm_mday = 26, tm.tm_wday = 6;
  tm.tm_hour = 14, tm.tm_min = 5;
  TEST_ASSERT_EQUAL_STRING("Seen 26 September 2026 at 14:05",
                           expandText("Seen {{date.long}} at {{time}}", &tm).c_str());
  TEST_ASSERT_EQUAL_STRING("Saturday 26 September 2026", expandText("{{ Date.Full }}", &tm).c_str());
  TEST_ASSERT_EQUAL_STRING("09/26/26 2:05 pm",
                           expandText("{{date.short}} {{time}}", &tm, TextPrefs{DateOrder::MonthFirst, true}).c_str());
  TEST_ASSERT_EQUAL_STRING("Sat 26 Sep 09 2026",
                           expandText("{{weekday.short}} {{day}} {{month.short}} {{month.number}} {{year}}", &tm).c_str());
  TEST_ASSERT_EQUAL_STRING("{{nope}} {{ unclosed", expandText("{{nope}} {{ unclosed", &tm).c_str());
  // One name a value: there is no bare {{date}} beside {{date.long}}.
  TEST_ASSERT_EQUAL_STRING("{{date}}", expandText("{{date}}", &tm).c_str());
  // {{time}} follows the preference; the other two are fixed whatever it says.
  TEST_ASSERT_EQUAL_STRING("2:05 pm 14:05", expandText("{{time.12h}} {{time.24h}}", &tm).c_str());
  TEST_ASSERT_EQUAL_STRING("2:05 pm 14:05",
                           expandText("{{time}} {{time.24h}}", &tm, TextPrefs{DateOrder::DayFirst, true}).c_str());
  TEST_ASSERT_EQUAL_STRING("14 2 pm {{hour}}",
                           expandText("{{hour.24h}} {{hour.12h}} {{hour}}", &tm, TextPrefs{DateOrder::DayFirst, true}).c_str());
  tm.tm_hour = 0;
  TEST_ASSERT_EQUAL_STRING("12:05 am", formatTime(tm, true).c_str());
  TEST_ASSERT_EQUAL_STRING("0", formatHour(tm, false).c_str());
  TEST_ASSERT_EQUAL_STRING("12 am", formatHour(tm, true).c_str());
  tm.tm_hour = 12;
  TEST_ASSERT_EQUAL_STRING("12:05 pm", formatTime(tm, true).c_str());
  TEST_ASSERT_EQUAL_STRING("12:05", formatTime(tm, false).c_str());
  tm.tm_hour = 14;
  TEST_ASSERT_EQUAL_STRING("", expandText("Seen {{date.long}}", nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("No clock {{nope}}", expandText("No clock {{nope}}", nullptr).c_str());
}

// Names beyond the clock's come from the caller: filled in, left as written
// when nobody knows them, and the whole line dropped for one there is none of.
void test_page_text_asks_the_caller_for_other_names() {
  const TextLookup lookup = [](const std::string &name, std::string &out) {
    if (name == "new") return out = "Galah and Crimson Rosella", TextValue::Filled;
    if (name == "battery") return TextValue::Missing;
    return TextValue::Unknown;
  };
  TEST_ASSERT_EQUAL_STRING("New today: Galah and Crimson Rosella!",
                           expandText("New today: {{ NEW }}!", nullptr, TextPrefs{}, lookup).c_str());
  TEST_ASSERT_EQUAL_STRING("", expandText("Battery {{battery}}", nullptr, TextPrefs{}, lookup).c_str());
  TEST_ASSERT_EQUAL_STRING("{{what}}", expandText("{{what}}", nullptr, TextPrefs{}, lookup).c_str());
  TEST_ASSERT_TRUE(textUses("Refresh {{ Refresh }}", "refresh"));
  TEST_ASSERT_FALSE(textUses("Refresh {{refreshes}} {{date}}", "refresh"));
}

// A line along an edge takes a band the birds are packed clear of: the text
// is drawn in its band and every bird in the room between.
void test_page_text_keeps_the_birds_out_of_its_band() {
  const std::vector<uint8_t> pack = v7FixturePack();
  Plates p;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(openPack(pack, p, &err), err.c_str());
  Font face;
  TEST_ASSERT_TRUE_MESSAGE(loadFont(face), "run from firmware/: needs ../assets/fonts");

  BirdPageSettings s;
  s.names = NameStyle::None;
  s.topText = "Seen near home";
  s.bottomText = "26 September 2026";
  BirdPageReport report;
  Frame out;
  TEST_ASSERT_TRUE(renderBirdPage(p, {0}, s, face, out, &report));
  TEST_ASSERT_TRUE(report.birdsTop > 0 && report.birdsBottom < out.h);
  const auto inked = [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < out.w; ++x)
        if (out.row(y)[x] != kWhite) return true;
    return false;
  };
  TEST_ASSERT_TRUE_MESSAGE(inked(0, report.birdsTop), "no top line above the birds");
  TEST_ASSERT_TRUE_MESSAGE(inked(report.birdsBottom, out.h), "no bottom line below the birds");
  for (const Placement &pl : report.placements) {
    // dim is the longest side; the bird's height is its share of that.
    const PlateEntry &e = p.entry(size_t(pl.index));
    const int h = pl.dim * e.h / std::max(e.w, e.h);
    TEST_ASSERT_TRUE(pl.y >= 0);
    TEST_ASSERT_TRUE(report.birdsTop + pl.y + h <= report.birdsBottom);
  }

  // Without them the birds have the page.
  BirdPageSettings bare;
  bare.names = NameStyle::None;
  BirdPageReport bareReport;
  TEST_ASSERT_TRUE(renderBirdPage(p, {0}, bare, face, out, &bareReport));
  TEST_ASSERT_TRUE(bareReport.birdsTop < report.birdsTop);
  TEST_ASSERT_TRUE(bareReport.birdsBottom > report.birdsBottom);
}

void test_a_pack_that_is_not_a_pack_is_refused() {
  std::vector<uint8_t> junk(2000, 0x42);
  Plates p;
  std::string err;
  const bool ok = p.open(
      [&junk](uint32_t offset, void *dst, size_t len) {
        if (size_t(offset) + len > junk.size()) return false;
        std::memcpy(dst, junk.data() + offset, len);
        return true;
      },
      &err);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_TRUE(!err.empty());
  TEST_ASSERT_EQUAL_INT(0, int(p.count()));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_each_ink_dithers_to_itself);
  RUN_TEST(test_paper_dithers_to_white_not_speckle);
  RUN_TEST(test_dithering_in_place_matches_a_separate_frame);
  RUN_TEST(test_a_sprites_clear_pixels_do_not_paint);
  RUN_TEST(test_a_grown_sprite_blends_between_its_pixels);
  RUN_TEST(test_wifi_qr_payload_escapes_what_the_format_reserves);
  RUN_TEST(test_a_qr_code_is_drawn_with_its_quiet_zone);
  RUN_TEST(test_vivid_spends_more_ink_on_a_tint_and_none_on_the_paper);
  RUN_TEST(test_sharpening_keeps_a_thin_line_and_leaves_the_paper_alone);
  RUN_TEST(test_edge_ink_draws_a_faint_boundary_and_nothing_else);
  RUN_TEST(test_edge_ink_favours_light_over_dark);
  RUN_TEST(test_a_name_pair_reserves_room_for_both_lines);
  RUN_TEST(test_the_plane_coder_agrees_with_the_bake_to_the_byte);
  RUN_TEST(test_the_plane_coder_round_trips_and_refuses_a_cut_stream);
  RUN_TEST(test_a_v7_pack_round_trips_through_the_reader);
  RUN_TEST(test_a_page_margin_leaves_its_border_untouched);
  RUN_TEST(test_page_text_fills_in_the_clock);
  RUN_TEST(test_page_text_asks_the_caller_for_other_names);
  RUN_TEST(test_page_text_keeps_the_birds_out_of_its_band);
  RUN_TEST(test_a_pack_that_is_not_a_pack_is_refused);
  return UNITY_END();
}
