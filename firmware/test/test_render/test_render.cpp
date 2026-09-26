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
#include "plates.h"
#include "render.h"

using namespace birdframe;

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
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:birdframe-3A7F;P:birdframe;;",
                           wifiQrPayload("birdframe-3A7F", "birdframe").c_str());
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

// A v5 pack built by hand, byte by byte, and read back: the format is the
// interface between the bake and the frame, and this is where it is pinned.
namespace {
void put16(std::vector<uint8_t> &b, uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void put32(std::vector<uint8_t> &b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
// A zlib stream with one stored block, which tinf inflates like any other.
std::vector<uint8_t> storedZlib(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> z{0x78, 0x01, 0x01};
  put16(z, uint32_t(data.size()));
  put16(z, uint32_t(~data.size() & 0xFFFF));
  z.insert(z.end(), data.begin(), data.end());
  uint32_t a = 1, s = 0;
  for (uint8_t d : data) { a = (a + d) % 65521; s = (s + a) % 65521; }
  const uint32_t adler = (s << 16) | a;
  z.push_back(adler >> 24); z.push_back((adler >> 16) & 255); z.push_back((adler >> 8) & 255); z.push_back(adler & 255);
  return z;
}
}  // namespace

void test_a_v6_pack_round_trips_through_the_reader() {
  // An 8x4 sprite, two chroma blocks. Inside the silhouette: row 0 x0-3,
  // row 1 x4-7, row 2 all, row 3 none; a nibble per pixel, 15 outside, else
  // the pixel's index inside (0-14, so the last inside pixel wraps to 0).
  std::vector<uint8_t> luma;
  {
    std::vector<uint8_t> n;
    int inside = 0;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 8; ++x) {
        const bool in = (y == 0 && x < 4) || (y == 1 && x >= 4) || y == 2;
        n.push_back(in ? uint8_t(inside++ % 15) : 15);
      }
    for (size_t i = 0; i < n.size(); i += 2) luma.push_back(uint8_t((n[i] << 4) | n[i + 1]));
  }
  const std::vector<uint8_t> chroma{0x01};  // block 0 -> code 0, block 1 -> code 1
  const std::vector<uint8_t> zl = storedZlib(luma), zc = storedZlib(chroma);

  std::vector<uint8_t> pack{'F', 'G', 'P', 'L'};
  put32(pack, 6);
  pack.push_back(4);  // depth
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

  Plates p;
  std::string err;
  const bool ok = p.open(
      [&pack](uint32_t offset, void *dst, size_t len) {
        if (size_t(offset) + len > pack.size()) return false;
        std::memcpy(dst, pack.data() + offset, len);
        return true;
      },
      &err);
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
  // (6,1): painted index 6 -> Y 96. Past the last block centre, so the
  // chroma is block 1's alone, not a blend: cb +50, cr -50.
  const int y = 96, r = y - 50, b = y + 50;
  const int g = int(std::lround((y - 0.299 * r - 0.114 * b) / 0.587));
  TEST_ASSERT_EQUAL_UINT16(rgb565(r, g, b), s.at(6, 1));
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
  RUN_TEST(test_a_sprites_clear_pixels_do_not_paint);
  RUN_TEST(test_a_grown_sprite_blends_between_its_pixels);
  RUN_TEST(test_wifi_qr_payload_escapes_what_the_format_reserves);
  RUN_TEST(test_a_qr_code_is_drawn_with_its_quiet_zone);
  RUN_TEST(test_vivid_spends_more_ink_on_a_tint_and_none_on_the_paper);
  RUN_TEST(test_sharpening_keeps_a_thin_line_and_leaves_the_paper_alone);
  RUN_TEST(test_edge_ink_draws_a_faint_boundary_and_nothing_else);
  RUN_TEST(test_edge_ink_favours_light_over_dark);
  RUN_TEST(test_a_name_pair_reserves_room_for_both_lines);
  RUN_TEST(test_a_v6_pack_round_trips_through_the_reader);
  RUN_TEST(test_a_pack_that_is_not_a_pack_is_refused);
  return UNITY_END();
}
