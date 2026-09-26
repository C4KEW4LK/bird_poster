// Packer, on the host. Mostly one shape of test: the bitset routines are
// rewrites of loops that were once written a pixel at a time, and the pixel-at-
// a-time version is kept here as the reference they have to agree with, bit for
// bit. That is the whole contract - the fast versions are an optimisation and
// nothing else, and a layout that moved would be a bug rather than a tuning
// choice, because `packing-example.png` and the fidelity table in the README
// are measured against where the birds actually land.
//
// The last test pins a whole layout to literal coordinates for the same reason.
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "packer.h"

using namespace birdposter;

namespace {

// xorshift32, so the masks are the same awkward shapes on every machine.
uint32_t gSeed = 0x9e3779b9u;
uint32_t next() {
  gSeed ^= gSeed << 13;
  gSeed ^= gSeed >> 17;
  gSeed ^= gSeed << 5;
  return gSeed;
}

// Something with a body, a thin spur and a few holes - a silhouette's awkward
// parts. A uniform random mask would erode to nothing and prove little.
Mask shape(int w, int h, uint32_t seed) {
  gSeed = seed;
  Mask m(w, h);
  const double cx = w * 0.42, cy = h * 0.5;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double dx = (x - cx) / (w * 0.38), dy = (y - cy) / (h * 0.34);
      if (dx * dx + dy * dy <= 1.0) m.set(x, y);
    }
  }
  for (int y = h * 2 / 5; y < h * 3 / 5; ++y)
    for (int x = w * 3 / 4; x < w; ++x) m.set(x, y);           // tail
  for (int y = h / 2; y < h; ++y)
    for (int x = w / 2; x < w / 2 + std::max(2, w / 30); ++x) m.set(x, y);  // leg
  for (int i = 0; i < w * h / 40; ++i)                          // and some gaps
    m.set(int(next() % uint32_t(w)), int(next() % uint32_t(h)));
  return m;
}

bool sameBits(const Mask& a, const Mask& b) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      if (a.get(x, y) != b.get(x, y)) return false;
  return true;
}

// --- the references, written the slow obvious way ------------------------

// A minimum filter over a binary image: a pixel survives only where the whole
// (2r+1) square was set, and off the edge counts as empty.
Mask erodedReference(const Mask& m, int radius) {
  if (m.empty() || radius <= 0) return m;
  Mask out(m.width(), m.height());
  for (int y = 0; y < m.height(); ++y) {
    for (int x = 0; x < m.width(); ++x) {
      bool keep = true;
      for (int dy = -radius; dy <= radius && keep; ++dy)
        for (int dx = -radius; dx <= radius && keep; ++dx) {
          const int sx = x + dx, sy = y + dy;
          if (sx < 0 || sx >= m.width() || sy < 0 || sy >= m.height() || !m.get(sx, sy))
            keep = false;
        }
      if (keep) out.set(x, y);
    }
  }
  return out;
}

Mask scaledReference(const Mask& m, int dim, bool flip) {
  if (m.empty() || dim <= 0) return Mask();
  const int longest = std::max(m.width(), m.height());
  const double s = double(dim) / double(longest);
  const int nw = std::max(1, int(std::lround(m.width() * s)));
  const int nh = std::max(1, int(std::lround(m.height() * s)));
  Mask out(nw, nh);
  for (int y = 0; y < nh; ++y) {
    const int sy = std::min(m.height() - 1, int((y + 0.5) / s));
    for (int x = 0; x < nw; ++x) {
      const int sx = std::min(m.width() - 1, int((x + 0.5) / s));
      if (m.get(flip ? m.width() - 1 - sx : sx, sy)) out.set(x, y);
    }
  }
  return out;
}

Sprite withLabelReference(int index, int dim, const Mask& art, const LabelBox& label, int gap) {
  Sprite s;
  s.index = index;
  s.dim = dim;
  if (label.w <= 0 || label.h <= 0) {
    s.mask = art;
    return s;
  }
  const int aw = art.width(), ah = art.height();
  long long sum = 0, count = 0;
  for (int y = 0; y < ah; ++y)
    for (int x = 0; x < aw; ++x)
      if (art.get(x, y)) sum += x, ++count;
  const double centre = count ? double(sum) / double(count) : aw / 2.0;
  const int offset = int(std::lround(centre - label.w / 2.0));
  const int left = std::min(0, offset);
  const int width = std::max(aw, offset + label.w) - left;
  const int ax = -left, lx = offset - left;
  const int from = std::max(0, lx - ax);
  const int to = std::min(aw, lx - ax + label.w);
  int bottom = -1;
  for (int y = ah - 1; y >= 0 && bottom < 0; --y)
    for (int x = from; x < to; ++x)
      if (art.get(x, y)) {
        bottom = y;
        break;
      }
  const int top = (bottom >= 0 ? bottom + 1 : ah) + gap;
  const int height = std::max(ah, top + label.h);
  s.mask = Mask(width, height);
  for (int y = 0; y < ah; ++y)
    for (int x = 0; x < aw; ++x)
      if (art.get(x, y)) s.mask.set(ax + x, y);
  for (int y = top; y < top + label.h; ++y)
    for (int x = lx; x < lx + label.w; ++x) s.mask.set(x, y);
  s.artX = ax;
  s.artY = 0;
  s.labelX = lx;
  s.labelY = top;
  s.labelW = label.w;
  s.labelH = label.h;
  s.hasLabel = true;
  return s;
}

bool collidesReference(const Mask& grid, const Mask& sprite, int x, int y) {
  for (int r = 0; r < sprite.height(); ++r)
    for (int c = 0; c < sprite.width(); ++c)
      if (sprite.get(c, r) && grid.get(x + c, y + r)) return true;
  return false;
}

// --- the tests -----------------------------------------------------------

void test_bit_parallel_erosion_matches_the_minimum_filter() {
  // Widths either side of a word boundary, because the shift carries between
  // words and the guard word is what makes the right-hand edge safe.
  const int sizes[][2] = {{1, 1}, {7, 5}, {63, 9}, {64, 9}, {65, 9}, {128, 31}, {129, 130}};
  for (const auto& wh : sizes) {
    const Mask m = shape(wh[0], wh[1], 0x1234u + uint32_t(wh[0]));
    for (int radius = 1; radius <= 3; ++radius) {
      TEST_ASSERT_TRUE_MESSAGE(sameBits(m.eroded(radius), erodedReference(m, radius)),
                               "eroded() disagrees with the per-pixel minimum filter");
    }
  }
}

void test_erosion_of_a_solid_block_eats_exactly_the_border() {
  Mask m(40, 40);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 40; ++x) m.set(x, y);
  const Mask e = m.eroded(kOverlapPx);
  // Both axes: the two passes are separate loops and an off-by-one in either
  // one of them shows up only on its own edge.
  TEST_ASSERT_FALSE(e.get(kOverlapPx - 1, 20));
  TEST_ASSERT_TRUE(e.get(kOverlapPx, 20));
  TEST_ASSERT_TRUE(e.get(39 - kOverlapPx, 20));
  TEST_ASSERT_FALSE(e.get(40 - kOverlapPx, 20));
  TEST_ASSERT_FALSE(e.get(20, kOverlapPx - 1));
  TEST_ASSERT_TRUE(e.get(20, kOverlapPx));
  TEST_ASSERT_TRUE(e.get(20, 39 - kOverlapPx));
  TEST_ASSERT_FALSE(e.get(20, 40 - kOverlapPx));
}

void test_rescale_matches_the_per_pixel_sampler() {
  const Mask m = shape(137, 91, 0xabcdu);
  for (int dim : {1, 12, 24, 90, 137, 200, 401}) {
    for (bool flip : {false, true}) {
      TEST_ASSERT_TRUE_MESSAGE(sameBits(m.scaled(dim, flip), scaledReference(m, dim, flip)),
                               "scaled() disagrees with the per-pixel sampler");
    }
  }
}

void test_a_label_joins_its_birds_collision_mask() {
  const Mask art = shape(120, 80, 0x5555u).eroded(kOverlapPx);
  for (const LabelBox& box : {LabelBox{40, 12}, LabelBox{200, 20}, LabelBox{5, 3}}) {
    const Sprite fast = withLabel(3, 77, art, box, 7);
    const Sprite slow = withLabelReference(3, 77, art, box, 7);
    TEST_ASSERT_TRUE_MESSAGE(sameBits(fast.mask, slow.mask), "the joined footprint moved");
    TEST_ASSERT_EQUAL_INT(slow.artX, fast.artX);
    TEST_ASSERT_EQUAL_INT(slow.artY, fast.artY);
    TEST_ASSERT_EQUAL_INT(slow.labelX, fast.labelX);
    TEST_ASSERT_EQUAL_INT(slow.labelY, fast.labelY);
    TEST_ASSERT_EQUAL_INT(slow.labelW, fast.labelW);
    TEST_ASSERT_EQUAL_INT(slow.labelH, fast.labelH);
  }
  // A name with no room above it still gets one: the box is reserved, never
  // painted afterwards.
  const Sprite s = withLabel(0, 10, art, LabelBox{40, 12}, 7);
  TEST_ASSERT_TRUE(s.hasLabel);
  for (int y = s.labelY; y < s.labelY + s.labelH; ++y)
    for (int x = s.labelX; x < s.labelX + s.labelW; ++x) TEST_ASSERT_TRUE(s.mask.get(x, y));
}

void test_word_wise_collision_matches_the_per_pixel_overlap() {
  const Mask sprite = shape(70, 40, 0x777u);
  Mask grid(300, 200);
  for (int i = 0; i < 60; ++i) grid.set(int(next() % 300u), int(next() % 200u));
  stamp(grid, shape(50, 50, 0x999u), 90, 60);
  for (int y = 0; y + sprite.height() <= grid.height(); y += 3)
    for (int x = 0; x + sprite.width() <= grid.width(); x += 3)
      TEST_ASSERT_EQUAL_MESSAGE(collidesReference(grid, sprite, x, y),
                                collides(grid, sprite, x, y),
                                "collides() disagrees with the per-pixel overlap");
}

// The invariant the whole packer exists for. Opaque pixels never overlap and
// nothing hangs off the canvas - including the reserved label boxes, which are
// part of the footprint rather than something drawn on top of it.
void test_a_pack_leaves_no_overlap_and_nothing_off_the_page() {
  std::vector<Sprite> sprites;
  for (int i = 0; i < 8; ++i) {
    const Mask art = shape(150 - 8 * i, 120 - 6 * i, 0x100u + uint32_t(i)).eroded(kOverlapPx);
    sprites.push_back(withLabel(i, 150 - 8 * i, art, LabelBox{60, 14}, 5));
  }
  std::vector<Placement> placed;
  TEST_ASSERT_TRUE(pack(sprites, 800, 600, placed, turnFor(1)));
  TEST_ASSERT_EQUAL_size_t(sprites.size(), placed.size());

  Mask grid(800, 600);
  for (size_t i = 0; i < placed.size(); ++i) {
    const Mask& m = sprites[i].mask;
    const int ox = placed[i].x - sprites[i].artX, oy = placed[i].y - sprites[i].artY;
    TEST_ASSERT_TRUE(ox >= 0 && oy >= 0);
    TEST_ASSERT_TRUE(ox + m.width() <= 800 && oy + m.height() <= 600);
    TEST_ASSERT_FALSE_MESSAGE(collides(grid, m, ox, oy), "two birds overlap");
    stamp(grid, m, ox, oy);
  }
}

// The sweep stops once its ellipse has cleared the page on both axes, since no
// angle is left that lands on it. A sprite that cannot fit has to say so rather
// than walk another two hundred laps off the paper - and it has to say so
// whether it is too wide, too tall, or merely too big.
void test_a_sprite_that_cannot_fit_fails_instead_of_sweeping() {
  const int tooBig[][2] = {{900, 100}, {100, 900}, {900, 900}};
  for (const auto& wh : tooBig) {
    std::vector<Sprite> sprites(1);
    sprites[0].index = 0;
    sprites[0].dim = wh[0];
    sprites[0].mask = Mask(wh[0], wh[1]);
    sprites[0].mask.set(wh[0] / 2, wh[1] / 2);
    std::vector<Placement> placed;
    TEST_ASSERT_FALSE(pack(sprites, 800, 600, placed, 0.0f));
  }
  // And one that only just fits still does.
  std::vector<Sprite> ok(1);
  ok[0].mask = Mask(800, 600);
  ok[0].mask.set(0, 0);
  std::vector<Placement> placed;
  TEST_ASSERT_TRUE(pack(ok, 800, 600, placed, 0.0f));
}

// The growth pass takes the room a bird has beside it: two small squares
// dropped far apart in a big box come out at the cap, still apart, still
// inside; a third wedged against the edge grows only as far as the edge lets it.
void test_grow_enlarges_birds_into_free_space_without_overlap() {
  std::vector<Mask> sources;
  for (int i = 0; i < 3; ++i) sources.push_back(shape(200, 200, 0x3000u + uint32_t(i)));
  const std::vector<bool> flips{false, false, false};
  const std::vector<LabelBox> labels;  // no names
  std::vector<Placement> placed;
  placed.push_back({0, 200, 300, 300, 0, 0, 0, 0, false});
  placed.push_back({1, 200, 1400, 1400, 0, 0, 0, 0, false});
  placed.push_back({2, 200, 1780, 900, 0, 0, 0, 0, false});  // 20 px from the right edge
  grow(sources, flips, labels, 0, 2000, 2000, placed, 1.5f);
  TEST_ASSERT_TRUE(placed[0].dim >= 290 && placed[0].dim <= 300);
  TEST_ASSERT_TRUE(placed[1].dim >= 290 && placed[1].dim <= 300);
  TEST_ASSERT_TRUE(placed[2].dim > 200);  // held by its right edge, it still grows
  Mask occ(2000, 2000);
  for (const Placement &p : placed) {
    const Mask m = sources[size_t(p.index)].scaled(p.dim, false).eroded(kOverlapPx);
    TEST_ASSERT_TRUE(p.x >= 0 && p.y >= 0 && p.x + m.width() <= 2000 && p.y + m.height() <= 2000);
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x)
        if (m.get(x, y)) {
          TEST_ASSERT_FALSE(occ.get(p.x + x, p.y + y));
          occ.set(p.x + x, p.y + y);
        }
  }
}

// Where four birds actually land, pinned, so a change anywhere above shows up
// here as moved coordinates rather than as a page that quietly looks different.
void test_a_whole_layout_lands_where_it_always_has() {
  std::vector<Mask> sources;
  std::vector<bool> flips;
  for (int i = 0; i < 4; ++i) {
    sources.push_back(shape(360 + 14 * i, 300 + 10 * i, 0x2000u + uint32_t(i)));
    flips.push_back(i % 3 == 0);
  }

  const auto size = pageSize(false);
  const int pageW = size.first, pageH = size.second;
  // The box these coordinates were pinned in: the 4% margin collage.py kept.
  // The page's own margin (kMargin) is a policy the packer does not own, and
  // changing it must not look like the packer moving.
  const int margin = int(std::lround(std::min(pageW, pageH) * 0.04f));
  const std::vector<LabelBox> labels(sources.size(), LabelBox{620, 130});

  std::vector<Placement> placed;
  int usedPx = 0;
  TEST_ASSERT_TRUE(layout(sources, flips, labels, 38, pageW, pageH, pageW - 2 * margin,
                          pageH - 2 * margin, placed, &usedPx, 1));
  TEST_ASSERT_EQUAL_INT(28, usedPx);
  TEST_ASSERT_EQUAL_size_t(4, placed.size());

  // index, dim, x, y, labelX, labelY, labelW, labelH
  // Every bird at one size, the first in the middle.
  const int expected[4][8] = {
      {0, 629, 451, 287, 700, 819, 174, 36},
      {1, 629, 115, 508, 320, 1037, 174, 36},
      {2, 629, 74, 31, 279, 558, 174, 36},
      {3, 629, 801, 55, 1050, 579, 174, 36},
  };
  for (size_t i = 0; i < placed.size(); ++i) {
    TEST_ASSERT_EQUAL_INT(expected[i][0], placed[i].index);
    TEST_ASSERT_EQUAL_INT(expected[i][1], placed[i].dim);
    TEST_ASSERT_EQUAL_INT(expected[i][2], placed[i].x);
    TEST_ASSERT_EQUAL_INT(expected[i][3], placed[i].y);
    TEST_ASSERT_EQUAL_INT(expected[i][4], placed[i].labelX);
    TEST_ASSERT_EQUAL_INT(expected[i][5], placed[i].labelY);
    TEST_ASSERT_EQUAL_INT(expected[i][6], placed[i].labelW);
    TEST_ASSERT_EQUAL_INT(expected[i][7], placed[i].labelH);
    TEST_ASSERT_TRUE(placed[i].hasLabel);
  }
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_bit_parallel_erosion_matches_the_minimum_filter);
  RUN_TEST(test_erosion_of_a_solid_block_eats_exactly_the_border);
  RUN_TEST(test_rescale_matches_the_per_pixel_sampler);
  RUN_TEST(test_a_label_joins_its_birds_collision_mask);
  RUN_TEST(test_word_wise_collision_matches_the_per_pixel_overlap);
  RUN_TEST(test_a_pack_leaves_no_overlap_and_nothing_off_the_page);
  RUN_TEST(test_a_sprite_that_cannot_fit_fails_instead_of_sweeping);
  RUN_TEST(test_grow_enlarges_birds_into_free_space_without_overlap);
  RUN_TEST(test_a_whole_layout_lands_where_it_always_has);
  return UNITY_END();
}
