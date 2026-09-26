// Silhouette packer, ported from src/birdframe/render/collage.py.
//
// The packer is the whole visual identity of the frame, so this is a faithful
// port rather than a reimplementation: same constants, same spiral, same
// largest-first order, same final centring. Where the Python uses numpy boolean
// arrays this uses bitsets - one bit per pixel instead of one byte - which is
// where the port's speed comes from: a 1200x1200 occupancy grid is 176 KB
// rather than 1.4 MB, and a collision test is a word-wise AND with early exit.
//
// Nothing here touches Arduino, PSRAM or the panel. It compiles for the host,
// which is the point: layout bugs are a one-second loop instead of
// flash-and-squint. See notes/esp32-port/IMPLEMENTATION.md §2 and §8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace birdframe {

// Ported verbatim from collage.py. These are not tunables - several were chosen
// to fix specific visual bugs, and the packer is not scale-invariant, so the
// pack always happens at kPackShort and placements are scaled afterwards.
constexpr int kPackShort = 1200;
// Page edge to content, on the short side. collage.py kept 4% clear; the
// glass is the whole page, so nothing is held back from its edge.
constexpr float kMargin = 0.0f;
constexpr int kOverlapPx = 2;     // erode the collision mask so birds nestle
constexpr int kStep = 6;          // spiral radius increment, and arc spacing
constexpr int kAttempts = 20;     // shrink tries before the layout gives up
// Bisections after the first fitting scale. The 0.9 walk can stop up to 11%
// below the largest size that fits; two halvings bring that under 3%. Kept the
// same as the Python's _REFINE_STEPS - the two are only comparable if they stop
// searching at the same point.
constexpr int kRefineSteps = 2;
constexpr int kProbeBands = 3;    // cheap pre-test rows per sprite
constexpr int kMinLabelPx = 11;      // page.MIN_LABEL_PX: below this a name is not a name
constexpr float kLabelGap = 0.35f;   // of the label's pixel size, bird to name
constexpr int kLabelRefPx = 100;     // the size label boxes are measured at when baked
// Sweep candidate positions on an ellipse matched to the page rather than a
// circle. Off restores the Python's exact behaviour, which is what the
// fidelity comparison in the README was measured against.
constexpr bool kEllipticalSpiral = true;

constexpr int kMinDim = 24;       // floor on a bird's longest side, in pack px

// Successive layouts step the spiral's start by the golden angle, so they land
// far apart on the circle instead of cycling through a handful of arrangements.
constexpr float kGoldenAngle = 2.399963f;

// The 13.3" panel is 4:3 and landscape-native, but which way the frame hangs on
// the wall is the owner's choice, so it is a setting rather than a constant -
// the same call the Pi frame's `settings.rotation` makes. Only the panel push
// turns pixels; everything above it just packs into the rectangle it is given,
// and a portrait page is a different arrangement rather than a rotated one.
constexpr int kPageLong = (kPackShort * 4) / 3;

inline std::pair<int, int> pageSize(bool portrait) {
  return portrait ? std::make_pair(kPackShort, kPageLong)
                  : std::make_pair(kPageLong, kPackShort);
}

// A 1-bit bitmap. Rows are padded to whole 64-bit words plus one guard word, so
// an unaligned read or write at the right-hand edge can always touch words[i+1]
// without a bounds test in the inner loop.
class Mask {
 public:
  Mask() = default;
  Mask(int w, int h);

  int width() const { return w_; }
  int height() const { return h_; }
  size_t stride() const { return stride_; }
  bool empty() const { return w_ <= 0 || h_ <= 0; }

  bool get(int x, int y) const;
  void set(int x, int y);
  const uint64_t *row(int y) const { return bits_.data() + size_t(y) * stride_; }
  uint64_t *row(int y) { return bits_.data() + size_t(y) * stride_; }

  // Nearest-neighbour rescale so the longest side becomes `dim`, optionally
  // mirrored. Mirroring, never rotation: a rotation would tilt the ground or
  // water on the plates that were drawn with terrain under the bird.
  Mask scaled(int dim, bool flip) const;

  // 5x5 minimum filter at radius 2 - PIL's MinFilter(2*r+1), which is what
  // _footprint applies. Erodes the silhouette so birds nestle into each other's
  // invisible paper halos while their bodies still never overlap.
  Mask eroded(int radius) const;

  int rowPopcount(int y) const;

 private:
  int w_ = 0, h_ = 0;
  size_t stride_ = 0;
  std::vector<uint64_t> bits_;
};

// A label's box, measured at kLabelRefPx when the sprite was baked. Text width
// is taken as linear in the font size, which is the same approximation the
// Python makes - it centres the redrawn name in the box it reserved precisely
// because a re-rasterised font is not exactly width x scale.
struct LabelBox {
  int w = 0;
  int h = 0;
};

// One bird as a packable unit - and its name, if it has one. `mask` is the whole
// footprint; `artAt` and `labelAt` locate the two inside it. Everything is in
// pack pixels: the art is redrawn from source at the size being rendered.
//
// The name is packed *with* the bird rather than placed afterwards. That is what
// guarantees it a place at all: the packer leaves no free paper between birds,
// so a name added later could only sit outside the cluster, and every interior
// bird would get none.
struct Sprite {
  int index = 0;
  int dim = 0;
  Mask mask;
  int artX = 0, artY = 0;
  int labelX = 0, labelY = 0;
  int labelW = 0;  // 0 when this sprite carries no name
  int labelH = 0;
  bool hasLabel = false;
};

struct Placement {
  int index = 0;
  int dim = 0;
  int x = 0;
  int y = 0;
  int labelX = 0;
  int labelY = 0;
  int labelW = 0;
  // Carried through so a caller can draw - and check - the box that was actually
  // reserved. Without it the harness guessed a height and drew a rectangle the
  // packer had never agreed to.
  int labelH = 0;
  bool hasLabel = false;
};

// Join a bird and its name into one packable footprint. The name is centred on
// the silhouette's column centroid - under the body, not out along the tail -
// and raised until it clears the outline, so it tucks into the gap beside a leg
// rather than floating below the whole bounding box.
Sprite withLabel(int index, int dim, const Mask &art, const LabelBox &label, int gap);

// True if `sprite` placed with its top-left at (x, y) overlaps anything already
// set in `grid`. Both are bitsets; the sprite is compared against an unaligned
// window of the grid, word at a time, with early exit on the first hit.
bool collides(const Mask &grid, const Mask &sprite, int x, int y);

// OR a placed sprite into the occupancy grid.
void stamp(Mask &grid, const Mask &sprite, int x, int y);

// Place every sprite with no opaque overlap and nothing off the canvas, or
// return false if one does not fit. Sprites must be pre-sorted largest-first.
bool pack(const std::vector<Sprite> &sprites, int width, int height,
          std::vector<Placement> &out, float turn = 0.0f);

// PROTOTYPE: coarse-to-fine contact packer. The spiral above probes blind and
// takes the first candidate that fits; this one sees every free position at
// once, on a 1/kCoarse occupancy grid, and picks the one with the most contact
// against what is already placed (and, weighted, the page edge), then settles
// it at full resolution within one coarse cell. The first sprite still takes
// the middle of the page. Same contract as `pack`.
constexpr int kCoarse = 8;        // full-res pixels per coarse cell
constexpr int kCoarseRing = 1;    // contact ring, in coarse cells
constexpr int kFineRing = 4;      // contact ring for the full-res settle, in px
constexpr int kFineWindow = kCoarse;  // +/- px searched around the coarse cell

// How a page is arranged. The three are different algorithms, not settings of
// one: `Classic` searches for a place for each bird, the other two decide the
// places up front and let the birds grow into them.
enum class PackStyle : uint8_t {
  // The spiral. Birds sorted largest-first are walked out from the middle of
  // the page and dropped in the first place they fit, all at one size. Densest
  // in the centre, and the arrangement the frame has always drawn.
  Classic = 0,
  // An even lattice, alternate rows offset half a cell, every bird at the
  // largest size that keeps its neighbours clear - then each grows into the
  // room around it. Fills a crowded page harder than the spiral can.
  Grid = 1,
  // The lattice shaken: seeds jittered and relaxed onto a centroidal Voronoi
  // diagram, each bird sized by the distance to its nearest neighbour, then
  // grown. Even like the lattice, without its rows.
  Scatter = 2,
  // One bird takes the middle of the page and is drawn large, the rest ring it
  // at even spacing and grow into what is left. The first bird given is the
  // one that gets the middle, so the caller's order decides the subject - on
  // the frame that is the bird its source ranked first.
  Hero = 3,
};

struct PackOptions {
  bool hero = false;     // one bird in the middle, the rest ringed around it
  // How much of the page the middle bird fills, against whichever pair of
  // edges its shape reaches first. 1.0 has it touch them; 0.8 is the default
  // because a hero at full height leaves the others crowded into the margins,
  // and the more birds there are the worse that reads.
  float heroFill = 0.8f;
  bool voronoi = false;  // centres on jittered, optionally Lloyd-relaxed seeds
  float jitter = 0.5f;   // how far a seed may stray from its cell, as a fraction of the cell
  int lloyd = 2;         // Lloyd relaxation passes: 0 leaves the jitter raw
  // Size each bird by the room around its own seed rather than giving the
  // whole set one scale. A pair of seeds that landed close start small and an
  // isolated one starts large, so a tight pair no longer decides the size of
  // every bird on the page; `grow` then evens out what it can.
  bool perSeed = false;
  bool cellSize = false; // measure that room as the Voronoi cell's area, not the nearest seed
  float sizeSpread = 2.0f;  // most a bird's weight may stray from the mean, either way
  bool grid = false;     // centres on an even lattice, nothing searched but the scale
  bool brick = false;    // with grid: offset alternate rows by half a cell
  bool coarse = false;   // score every coarse cell by contact
  bool pocket = false;   // candidates are the free pockets (distance-transform maxima), settled by contact
  bool airy = false;     // with pocket: sit in the middle of the biggest pocket instead of settling
  float borderWeight = 1.0f;  // contact against the page edge, relative to a neighbour
  float centreWeight = 0.25f; // pull toward the page centre, in units of the ring's cells
  // How finely the scale search closes on the largest size that fits. The
  // 0.9 walk can stop 11% short; each bisection halves what is left.
  int refineSteps = kRefineSteps;
  // Layouts to try at each scale, differing only in how ties are broken. A
  // greedy packer is decided by its first few placements, so a second start is
  // a different page rather than the same one again - and a scale counts as
  // fitting if any start fits it.
  int tries = 1;
  // Pull the cluster in on itself after it is packed: every bird steps toward
  // the middle until it touches. The pack leaves birds where they first fitted,
  // which is not where they would have settled.
  bool compact = false;
  // Place the biggest footprint first rather than in the order given. The
  // order given is the caller's precedence for the middle of the page, so this
  // trades that for the easier packing problem.
  bool bigFirst = false;
  uint32_t seed = 0;  // which of `tries` this is
  bool any() const { return coarse || pocket || grid || voronoi || hero; }
};

bool packCoarse(const std::vector<Sprite> &sprites, int width, int height,
                std::vector<Placement> &out, const PackOptions &opt = PackOptions());

// PROTOTYPE: the lattice. Every bird is centred in its own cell of an evenly
// spaced grid matched to the page's aspect, and nothing is searched but the
// scale: the layout is the largest size at which no two neighbours touch.
// There is no cleverness here at all, which is the point - the packing comes
// afterwards, from `grow`, as each bird swells into the room around it until
// it hits something. Same contract as `pack`.
bool packGrid(const std::vector<Sprite> &sprites, int width, int height,
              std::vector<Placement> &out, const PackOptions &opt = PackOptions());

// PROTOTYPE: the lattice, shaken. Seeds start on the same even grid and are
// then offset at random, which breaks the rows the lattice reads as - and
// then, because random offsets clump as readily as they spread, each seed is
// pulled to the centre of the region nearer to it than to any other. That is
// Lloyd's relaxation, and its fixed point is a centroidal Voronoi diagram:
// points as evenly spread as a lattice, with none of its regularity. The birds
// go on the seeds and `grow` does the packing.
bool packVoronoi(const std::vector<Sprite> &sprites, int width, int height,
                 std::vector<Placement> &out, const PackOptions &opt = PackOptions());

// PROTOTYPE: the hero page, built in the order it is described. One bird goes
// in the middle, grown until it touches the page's edges - `layout` sizes it
// off the paper rather than off the set, so it is the page that stops it, not
// its neighbours. The rest are then laid around it on evenly spaced rays, each
// walking out from the centre until it clears what is already placed, so they
// ring whatever shape the hero turned out to be. `grow` then fills what is
// left. Same contract as `pack`.
bool packHero(const std::vector<Sprite> &sprites, int width, int height,
              std::vector<Placement> &out, const PackOptions &opt = PackOptions());

// The seeds `packVoronoi` will use, and what each one is worth in size: the
// room around it, as a multiple of the average. Deterministic in `n`, the page
// and the options, which is what lets the scale search size the birds and the
// packer place them without passing anything between them.
void voronoiSeeds(int n, int width, int height, const PackOptions &opt, std::vector<double> &seedX,
                  std::vector<double> &seedY, std::vector<double> &weight);

// Pull every bird toward the cluster's centre until it touches something,
// a step at a time, for `rounds` passes or until nothing moves. Run after a
// pack, before centring.
void compact(const std::vector<Sprite> &sprites, std::vector<Placement> &placed, int boxW,
             int boxH, int rounds = 4, int step = 4);

// Shift the packed cluster so its bounding box is centred on the canvas.
void center(std::vector<Placement> &placed, const std::vector<Sprite> &sprites,
            int width, int height);

// Everything a style decides: how the birds are placed, and how they are then
// allowed to grow. `grow`'s arguments are here rather than in PackOptions
// because growth is a separate pass over a finished layout.
struct PackPlan {
  PackOptions pack;
  float growMax = 1.5f;    // most a bird may end up above its packed size
  int growNudge = 0;       // px it may step aside to find room; 0 holds it still
  int growRounds = 1;      // passes over the set, re-based each time
  float growStep = 0.0f;   // most it may gain in one round; 0 lets it take all it can
};

// The plan a style means. One place to read what each of the three does.
PackPlan planFor(PackStyle style);

// Where on the ellipse a layout starts its sweep. The spiral tries candidates in
// angle order, so the first bird placed at each radius lands wherever the sweep
// begins; moving that start rearranges the whole cluster at no cost to packing
// quality, since every candidate is still tried.
float turnFor(int layout);

// Mirror a bird or not, for a given layout. Layout 0 returns the flip baked into
// the mask file, which is the Python's blake2b answer - so the reference page is
// byte-identical. Later layouts use a cheap FNV-1a over the same name, because
// blake2b on device would cost more than the variety is worth: the *rule* is the
// port (each bird decided by its own name, so an arrival cannot re-roll its
// neighbours), the digest is not. Documented divergence; see firmware/README.md.
bool flipFor(bool baked, const char *name, int layout);

// The whole layout: shrink the set to the first size where every bird fits, and
// return its placements, in box coordinates. `sources` are full-resolution
// silhouettes; each attempt rescales them, because the packer works in whole
// pixels and a mask eroded at one size is not the same shape as one eroded at
// another.
//
// Two sizes, and the difference matters: birds are *sized* off the whole page
// but *packed* inside the margin, so only a set that genuinely does not fit has
// to shrink. Sizing off the box instead makes every bird about 3% too small.
// `labels` may be empty, which means names are off. `namePx` is the size a name
// is asked for; names shrink with the birds, because a fixed-size name never
// yields and a full page of them then cannot converge at all. The size they
// actually landed at comes back in `usedPx`.
// Every bird is drawn at the same size - one scale for the set, found by the
// search - and the first in `sources` is placed first, so the order given is
// the order of precedence for the middle of the page.
bool layout(const std::vector<Mask> &sources, const std::vector<bool> &flips,
            const std::vector<LabelBox> &labels, int namePx, int pageW, int pageH, int boxW,
            int boxH, std::vector<Placement> &out, int *usedPx, int variant = 0,
            const PackOptions &opt = PackOptions());

// After a layout fits, let each bird grow where it stands. The scale search
// stops the whole set at the size the *tightest* bird runs out of room, so
// the others are left smaller than the space beside them allows. This takes
// them one at a time, smallest first, and bisects each up toward
// `maxFactor` times its packed size - centre held, then each edge held in
// turn - accepting the largest that fits the box and touches nothing. Names
// stay at `namePx`. A separate pass on purpose: `layout` is a port pinned
// against its reference, and this is what happens after it.
// `nudge` > 0 (PROTOTYPE) also tries shifting the bird up to that many pixels
// in every direction at each size, so a bird stopped by a neighbour on one
// side can step away from it and keep growing. `rounds` > 1 repeats the whole
// pass, each time re-based on where the birds now stand, so those steps
// compound and a bird held by a neighbour gets another go once it has moved.
// `maxFactor` bounds the total growth, not each round's. `roundStep` > 1 caps
// what a bird may gain in one round, which changes the character of the pass:
// bisecting straight to the cap lets the first bird into a gap take all of it,
// while a small step taken round-robin grows the set together and keeps the
// birds near one size. It needs `rounds` to reach the same total.
void grow(const std::vector<Mask> &sources, const std::vector<bool> &flips,
          const std::vector<LabelBox> &labels, int namePx, int boxW, int boxH,
          std::vector<Placement> &placed, float maxFactor = 1.5f, int nudge = 0,
          int rounds = 1, float roundStep = 0.0f);

}  // namespace birdframe
