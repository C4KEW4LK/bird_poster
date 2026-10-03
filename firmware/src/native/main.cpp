// Host harness for the packer (build order step 2, IMPLEMENTATION.md §8).
//
//   pio run -e native && .pio/build/native/program masks.bin out.pgm
//
// Packs the baked silhouettes at the same 1200-pixel short side the Python uses
// and writes the occupancy grid as a PGM, so a layout can be looked at without
// flashing anything. The numbers it prints - median bird size, pack time - are
// the ones to compare against the Python original.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "packer.h"

using namespace birdposter;

namespace {

struct Baked {
  std::string name;
  Mask mask;
  bool flip = false;
  LabelBox label;  // measured at kLabelRefPx when baked
};

template <typename T>
bool readPod(std::FILE *f, T &out) {
  return std::fread(&out, sizeof(T), 1, f) == 1;
}

bool load(const char *path, std::vector<Baked> &out) {
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return false;
  }
  uint32_t magic = 0, version = 0, count = 0;
  if (!readPod(f, magic) || !readPod(f, version) || !readPod(f, count) ||
      magic != 0x4B4D4746u || version != 3u) {
    std::fprintf(stderr, "%s is not a v3 FGMK file (re-run bake_masks.py)\n", path);
    std::fclose(f);
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t nameLen = 0, w = 0, h = 0;
    uint8_t flip = 0;
    if (!readPod(f, nameLen)) break;
    std::string name(nameLen, '\0');
    if (std::fread(name.data(), 1, nameLen, f) != nameLen) break;
    uint16_t labelW = 0, labelH = 0;
    if (!readPod(f, w) || !readPod(f, h) || !readPod(f, flip)) break;
    if (!readPod(f, labelW) || !readPod(f, labelH)) break;

    Baked b;
    b.label = {labelW, labelH};
    b.name = name;
    b.flip = flip != 0;
    b.mask = Mask(w, h);
    const size_t stride = (size_t(w) + 7) / 8;
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < h; ++y) {
      if (std::fread(row.data(), 1, stride, f) != stride) break;
      for (int x = 0; x < w; ++x)
        if (row[x >> 3] & (0x80 >> (x & 7))) b.mask.set(x, y);
    }
    out.push_back(std::move(b));
  }
  std::fclose(f);
  return !out.empty();
}

// The packed silhouettes as grey on paper, so a layout can be eyeballed. Not
// the collage: the plates and the labels are a later step, and the whole point
// of looking at this one is the shape of the arrangement.
bool writePgm(const char *path, const std::vector<Placement> &placed,
              const std::vector<Baked> &birds, int width, int height) {
  std::vector<uint8_t> page(size_t(width) * height, 242);  // TARGET_PAPER's red
  for (const Placement &p : placed) {
    const Mask m = birds[p.index].mask.scaled(p.dim, birds[p.index].flip);
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x)
        if (m.get(x, y)) {
          const int px = p.x + x, py = p.y + y;
          if (px >= 0 && py >= 0 && px < width && py < height)
            page[size_t(py) * width + px] = 60;
        }
  }
  for (const Placement &p : placed) {
    if (!p.hasLabel) continue;
    const int lh = p.labelH;  // the box the packer actually reserved
    for (int y = p.labelY; y < p.labelY + lh && y < height; ++y)
      for (int x = p.labelX; x < p.labelX + p.labelW && x < width; ++x)
        if (x >= 0 && y >= 0) page[size_t(y) * width + x] = 150;
  }
  std::FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P5\n%d %d\n255\n", width, height);
  std::fwrite(page.data(), 1, page.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace

int pageMain(int argc, char **argv);  // page.cpp: the full page from a plate pack

// Which of the two bakes this is. A plate pack renders the real page; a mask
// file draws the layout as grey silhouettes, which is the faster loop when the
// question is where the birds land.
bool isPlatePack(const char *path) {
  std::FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  uint32_t magic = 0;
  const bool ok = std::fread(&magic, sizeof magic, 1, f) == 1;
  std::fclose(f);
  return ok && magic == 0x4C504746u;  // 'FGPL'
}

int main(int argc, char **argv) {
  const char *in = argc > 1 ? argv[1] : "masks.bin";
  if (argc > 1 && (isPlatePack(in) || std::strcmp(in, "--setup") == 0 ||
                   std::strcmp(in, "--status") == 0))
    return pageMain(argc, argv);
  const char *out = argc > 2 ? argv[2] : "layout.pgm";
  // Portrait is a different page, not a turned one - the packer fills whatever
  // rectangle it is handed, so the two are worth looking at side by side.
  bool portrait = false;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--portrait") == 0) portrait = true;
  const auto [width, height] = pageSize(portrait);

  std::vector<Baked> birds;
  if (!load(in, birds)) return 1;

  bool names = true;
  int variant = 0;
  bool doGrow = false;
  // The three styles the frame offers; the flags below them are the knobs the
  // styles are built out of, kept so a layout can still be taken apart here.
  PackPlan plan = planFor(PackStyle::Classic);
  int growNudge = plan.growNudge, growRounds = plan.growRounds;
  float growMax = plan.growMax, growStep = plan.growStep;
  PackOptions opt = plan.pack;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-names") == 0) names = false;
    if (std::strcmp(argv[i], "--grow") == 0) doGrow = true;
    if (std::strcmp(argv[i], "--no-grow") == 0) doGrow = false;
    if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      plan = planFor(!std::strcmp(v, "grid")      ? PackStyle::Grid
                     : !std::strcmp(v, "scatter") ? PackStyle::Scatter
                     : !std::strcmp(v, "hero")    ? PackStyle::Hero
                                                  : PackStyle::Classic);
      opt = plan.pack;
      growNudge = plan.growNudge, growRounds = plan.growRounds;
      growMax = plan.growMax, growStep = plan.growStep;
      doGrow = true;  // the placed styles are only half a layout without it
    }
    if (std::strcmp(argv[i], "--coarse") == 0) opt.coarse = true;
    if (std::strcmp(argv[i], "--pocket") == 0) opt.pocket = true;
    if (std::strcmp(argv[i], "--airy") == 0) opt.pocket = opt.airy = true;
    if (std::strcmp(argv[i], "--nudge") == 0 && i + 1 < argc) growNudge = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) growRounds = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--maxgrow") == 0 && i + 1 < argc) growMax = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--fair") == 0 && i + 1 < argc) growStep = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--refine") == 0 && i + 1 < argc) opt.refineSteps = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--tries") == 0 && i + 1 < argc) opt.tries = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--compact") == 0) opt.compact = true;
    if (std::strcmp(argv[i], "--grid") == 0) opt.grid = true;
    if (std::strcmp(argv[i], "--brick") == 0) opt.grid = opt.brick = true;
    if (std::strcmp(argv[i], "--voronoi") == 0) opt.voronoi = true;
    if (std::strcmp(argv[i], "--jitter") == 0 && i + 1 < argc) opt.jitter = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--lloyd") == 0 && i + 1 < argc) opt.lloyd = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--perseed") == 0) opt.voronoi = opt.perSeed = true;
    if (std::strcmp(argv[i], "--cellsize") == 0) opt.cellSize = true;
    if (std::strcmp(argv[i], "--herofill") == 0 && i + 1 < argc) opt.heroFill = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--herolabel") == 0 && i + 1 < argc) opt.heroLabel = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--spread") == 0 && i + 1 < argc) opt.sizeSpread = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) opt.seed = uint32_t(std::atoi(argv[++i]));
    if (std::strcmp(argv[i], "--bigfirst") == 0) opt.bigFirst = true;
    if (std::strcmp(argv[i], "--border") == 0 && i + 1 < argc) opt.borderWeight = float(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--centre") == 0 && i + 1 < argc) opt.centreWeight = float(std::atof(argv[++i]));
    // Which arrangement of this set to draw. Same birds, different number,
    // different page - the frame advances it whenever it re-renders.
    if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) variant = std::atoi(argv[++i]);
  }

  std::vector<Mask> sources;
  std::vector<bool> flips;
  std::vector<LabelBox> labels;
  for (const Baked &b : birds) {
    sources.push_back(b.mask);
    flips.push_back(flipFor(b.flip, b.name.c_str(), variant));
    if (names) labels.push_back(b.label);
  }

  // Pack inside the margin but size off the whole page, so only a set that does
  // not fit has to shrink.
  const int margin = int(std::lround(std::min(width, height) * kMargin));
  const int boxW = width - 2 * margin, boxH = height - 2 * margin;

  // page.label_px: a fraction of the short side, so a name holds its proportion
  // at any resolution. "medium" is 0.032. Off the *page*, like the bird sizing -
  // measuring it off the box asks for a name 8% too small and the whole layout
  // then converges somewhere else.
  const int namePx = std::max(kMinLabelPx, int(std::lround(std::min(width, height) * 0.032)));

  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--no-grow") == 0) doGrow = false;

  std::vector<Placement> placed;
  int usedPx = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok =
      layout(sources, flips, labels, namePx, width, height, boxW, boxH, placed, &usedPx,
             variant, opt);
  if (ok && doGrow) grow(sources, flips, labels, usedPx, boxW, boxH, placed, growMax, growNudge, growRounds, growStep);
  const auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!ok) {
    std::fprintf(stderr, "no layout fits %zu birds in %dx%d\n", birds.size(), boxW, boxH);
    return 2;
  }
  for (Placement &p : placed) {
    p.x += margin, p.y += margin;
    p.labelX += margin, p.labelY += margin;
  }

  // Two different kinds of empty page, and they want different fixes. Ink is
  // how much paper the silhouettes actually cover; span is how much of the page
  // the cluster's bounding box reaches. Low ink with high span means the pack is
  // loose; low span means the birds were simply sized too small.
  long ink = 0, boxes = 0;
  int cx0 = width, cy0 = height, cx1 = 0, cy1 = 0;
  for (const Placement &p : placed) {
    const Mask m = sources[p.index].scaled(p.dim, flips[p.index]);
    boxes += long(m.width()) * m.height();
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x)
        if (m.get(x, y)) ++ink;
    cx0 = std::min(cx0, p.x);
    cy0 = std::min(cy0, p.y);
    cx1 = std::max(cx1, p.x + m.width());
    cy1 = std::max(cy1, p.y + m.height());
  }
  const double page = double(width) * height;
  // Box coverage is the sum of the birds' bounding boxes over the page, so it
  // reads the packed scale directly and can pass 100% when silhouettes nest.
  std::printf("  box coverage %.1f%% of page\n", 100.0 * boxes / page);
  std::printf("  ink %.1f%% of page   cluster span %.1f%% (%dx%d of %dx%d)\n", 100.0 * ink / page,
              100.0 * double(cx1 - cx0) * (cy1 - cy0) / page, cx1 - cx0, cy1 - cy0, width, height);

  std::vector<int> dims;
  for (const Placement &p : placed) dims.push_back(p.dim);
  std::sort(dims.begin(), dims.end());

  // Two kinds of evenness, because they fail separately: birds can be all one
  // size but bunched into half the page, or spread over it at wildly different
  // sizes. Both are coefficients of variation, so 0 is perfectly even.
  double mean = 0;
  for (int d : dims) mean += d;
  mean /= double(dims.size());
  double var = 0;
  for (int d : dims) var += (d - mean) * (d - mean);
  const double sizeCv = mean > 0 ? std::sqrt(var / double(dims.size())) / mean : 0.0;

  // Ink per cell of a 4x3 grid over the page: how evenly the birds cover it.
  const int gx = portrait ? 3 : 4, gy = portrait ? 4 : 3;
  std::vector<long> cell(size_t(gx) * gy, 0);
  for (const Placement &p : placed) {
    const Mask m = sources[p.index].scaled(p.dim, flips[p.index]);
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x) {
        if (!m.get(x, y)) continue;
        const int px = p.x + x, py = p.y + y;
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        cell[size_t(py * gy / height) * gx + size_t(px * gx / width)] += 1;
      }
  }
  double cmean = 0;
  for (long c : cell) cmean += double(c);
  cmean /= double(cell.size());
  double cvar = 0;
  for (long c : cell) cvar += (double(c) - cmean) * (double(c) - cmean);
  const double spreadCv = cmean > 0 ? std::sqrt(cvar / double(cell.size())) / cmean : 0.0;
  std::printf("  evenness: size cv %.3f, page cv %.3f (0 is even)\n", sizeCv, spreadCv);

  // A reserved name must never land on another bird. withLabel welds the box
  // into the bird's own collision mask, so the packer cannot place a neighbour
  // over it - but that is a claim worth testing rather than trusting, and it is
  // exactly the failure a reader would spot first in the PGM.
  long labelOnBird = 0;
  for (size_t i = 0; i < placed.size(); ++i) {
    if (!placed[i].hasLabel) continue;
    for (size_t j = 0; j < placed.size(); ++j) {
      if (i == j) continue;
      // Eroded, because that is what the packer collides on: a bird may nestle
      // kOverlapPx into a neighbour's invisible paper halo, so comparing against
      // the full silhouette reports that halo as an overlap it never promised.
      const Mask m =
          sources[placed[j].index].scaled(placed[j].dim, flips[placed[j].index]).eroded(kOverlapPx);
      for (int y = 0; y < placed[i].labelH; ++y) {
        const int gy = placed[i].labelY + y - placed[j].y;
        if (gy < 0 || gy >= m.height()) continue;
        for (int x = 0; x < placed[i].labelW; ++x) {
          const int gx = placed[i].labelX + x - placed[j].x;
          if (gx >= 0 && gx < m.width() && m.get(gx, gy)) ++labelOnBird;
        }
      }
    }
  }
  std::printf("  label box over another bird: %ld px%s\n", labelOnBird,
              labelOnBird ? "   <-- BUG: a name is sitting on a neighbour"
                          : " (none - the reservation holds)");

  // Bounding boxes are expected to overlap heavily - that is the whole point.
  // Collision is tested against the eroded alpha silhouette, so birds nest
  // inside each other's boxes; if this ever reads 0 the packer has quietly
  // become a rectangle packer.
  long boxOverlap = 0;
  int overlapping = 0;
  for (size_t i = 0; i < placed.size(); ++i) {
    for (size_t j = i + 1; j < placed.size(); ++j) {
      const Mask a = sources[placed[i].index].scaled(placed[i].dim, flips[placed[i].index]);
      const Mask b = sources[placed[j].index].scaled(placed[j].dim, flips[placed[j].index]);
      const int x0 = std::max(placed[i].x, placed[j].x);
      const int y0 = std::max(placed[i].y, placed[j].y);
      const int x1 = std::min(placed[i].x + a.width(), placed[j].x + b.width());
      const int y1 = std::min(placed[i].y + a.height(), placed[j].y + b.height());
      if (x1 > x0 && y1 > y0) {
        ++overlapping;
        boxOverlap += long(x1 - x0) * (y1 - y0);
      }
    }
  }
  std::printf("%zu birds packed in %.0f ms at %dx%d (%s)\n", placed.size(), ms, width, height,
              portrait ? "portrait" : "landscape");
  std::printf("  silhouette packing: %d of %zu box pairs overlap, %ld px of box area shared\n",
              overlapping, placed.size() * (placed.size() - 1) / 2, boxOverlap);
  if (!labels.empty())
    std::printf("  labels reserved at %d px (asked %d)\n", usedPx, namePx);
  std::printf("  bird size  median %d px, smallest %d, largest %d\n",
              dims[dims.size() / 2], dims.front(), dims.back());
  for (const Placement &p : placed)
    std::printf("  %-32s %4d px at (%4d,%4d)\n", birds[p.index].name.c_str(), p.dim, p.x, p.y);

  if (!writePgm(out, placed, birds, width, height)) {
    std::fprintf(stderr, "cannot write %s\n", out);
    return 3;
  }
  std::printf("wrote %s\n", out);
  return 0;
}
