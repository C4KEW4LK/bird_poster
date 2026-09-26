// The three things the glass can show.
//
//   birds     the collage: the page the frame exists to draw
//   setup     "join this network to set me up", with a QR code a phone reads
//   status    where the frame thinks it is: address, source, whether the
//             endpoint answered, the settings it is running under
//
// Each composes into a Frame at the page's own size; the panel push turns it
// to fit the glass. Arduino-free, so the host harness draws all three.
#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "plates.h"
#include "render.h"

namespace birdposter {

// page.LABEL_SIZES: a name as a fraction of the page's short side.
enum class LabelSize { Small, Medium, Large, XLarge };
float labelScale(LabelSize size);

// A name under a bird is the common name with the scientific one beneath it,
// smaller. The pack does not know common names - they come from the source,
// in whatever language it answers in - so they arrive with the request and
// their boxes are measured here rather than read from the pack. A bird
// without one gets the scientific name alone, in the baked box.
constexpr float kSubNameScale = 0.7f;

// What goes under a bird. A bird with no common name to hand gets its
// scientific name in the modes that would have shown one.
enum class NameStyle : uint8_t { Both = 0, Scientific = 1, Common = 2, None = 3 };

// How the common name is cased. The scientific name is never touched: its
// casing is part of the name.
enum class NameCase : uint8_t { AsGiven = 0, Upper = 1, Lower = 2 };
std::string cased(const std::string &name, NameCase how);

// The date on the bird page: how it is written, and which edge and end of the
// page it sits on. The numeric styles follow DateOrder; the ones in words are
// always day first.
enum class DateStyle : uint8_t {
  NumericShort = 0,  // 26/09/26, or 09/26/26 month first
  NumericLong = 1,   // 26/09/2026, or 09/26/2026 month first
  WordsShort = 2,    // 26 Sep 2026
  WordsLong = 3,     // 26 September 2026
  WordsFull = 4,     // Saturday 26 September 2026
};
enum class DateOrder : uint8_t { DayFirst = 0, MonthFirst = 1 };  // UK/AU, US
enum class DateEdge : uint8_t { Top = 0, Bottom = 1 };
enum class DateAlign : uint8_t { Left = 0, Centre = 1, Right = 2 };
std::string formatDate(const std::tm &tm, DateStyle style,
                       DateOrder order = DateOrder::DayFirst);

struct BirdPageSettings {
  bool portrait = false;
  NameStyle names = NameStyle::Both;
  NameCase commonCase = NameCase::Upper;
  LabelSize labelSize = LabelSize::Medium;
  // The scientific name's size under a common name, as a percentage of the
  // common name's. Alone - no common name, or the scientific style - it is
  // drawn at the full size whatever this says.
  int subNamePercent = int(kSubNameScale * 100 + 0.5f);
  int variant = 0;  // which arrangement of this set; the frame advances it
  int vivid = 2;    // colour boost before the dither, 0..kVividLevels-1; see render.h
  int sharpen = 2;  // unsharp mask before the dither, 0..kSharpenLevels-1
  int edges = 2;    // ink along detected edges, 0..kEdgeLevels-1
  // How a bird drawn at more than 1.5x its baked size is filled in; see
  // Resample. Anything scaled less is bilinear - the difference is lost there.
  Resample resample = Resample::CatmullRom;
  int jitter = 24;  // randomises where the dither's dots fall; see dither()
  // Print the page on cream rather than white. The whole canvas
  // is multiplied by a warm tone before the dither, as ink on tinted paper
  // takes the paper's colour: the background becomes the cream, the plates'
  // own pale paper around each bird becomes the same cream so the birds sit
  // into the page, and dark ink barely moves. 0 is off, 4 the warmest.
  int cream = 0;
  bool grow = true;  // let birds grow into the gaps after the pack (off: the packed layout alone)
  PackStyle packStyle = PackStyle::Classic;  // how the page is arranged; see packer.h
  // Written along one edge of the page in the label font, at the name size,
  // with the birds packed into what is left. Empty means no date.
  std::string date;
  DateEdge dateEdge = DateEdge::Bottom;
  DateAlign dateAlign = DateAlign::Right;
  // A short line in the same strip as the date, smaller, at the other end of
  // it - the refresh counter. Without a date it takes the strip on its own,
  // on the date's edge. Empty means none.
  std::string note;
  // Parallel to the plate indices; shorter, or an empty string, means none.
  std::vector<std::string> commonNames;
  // The face the common name is set in. Null, or not loaded, means the label
  // font - the scientific name is always in that.
  const Font *commonFont = nullptr;
  // A sprite for a page position from somewhere other than the pack - the
  // full-size plates the frame fetches over the network, or a host harness
  // experiment - returning false to fall back to the pack's. Asked only for a
  // bird drawn more than 1.25x its baked size, where a larger source shows;
  // drawn on the pack's own silhouette and layout, scaled to the same box.
  std::function<bool(size_t index, SpriteImage &out)> spriteOverride;
  // Host harness only: shown the canvas as the dither is about to see it -
  // the birds drawn and scaled, the paper tinted - for looking at.
  std::function<void(const Canvas &)> beforeDither;
  // Told what the render is doing as it goes, for a progress display. May be
  // empty.
  std::function<void(const char *)> progress;
};

// The box for a name at `px`, in the same terms the pack measures a single
// line: `first` (in `firstFont`) over `second` (in `secondFont`) at
// `subScale` of the size, wide as the wider line and tall as both. Either line
// may be empty, which makes it a one-line box.
LabelBox nameBox(const Font &firstFont, const Font &secondFont, const std::string &first,
                 const std::string &second, int px, float subScale = kSubNameScale);

// Draw a name centred in its box at `px`: `first` over `second`, the second
// line at `subScale` of it. Either may be empty for a single centred line.
void drawName(Frame &frame, const Font &firstFont, const Font &secondFont,
              const std::string &first, const std::string &second, int boxX, int boxY, int boxW,
              int boxH, int px, float subScale = kSubNameScale);

struct BirdPageReport {
  int placed = 0;
  int labelPx = 0;       // the size names actually landed at
  int packMs = 0;
  int drawMs = 0;
  int ditherMs = 0;
  int medianDim = 0;
};

// Draw `plateIndices` (into `plates`, page order - the caller has already
// chosen and ranked them) as the collage. Returns false if no layout fits,
// which is a bug rather than a tight page, or if a sprite fails to load.
bool renderBirdPage(const Plates &plates, const std::vector<int> &plateIndices,
                    const BirdPageSettings &settings, const Font &font, Frame &out,
                    BirdPageReport *report = nullptr);

// The access-point page. `url` is where the settings live once joined.
// `note` replaces the first line when there is a reason to give - the home
// network that could not be joined, say - rather than "not set up yet".
void renderSetupPage(bool portrait, const Font &font, const std::string &ssid,
                     const std::string &pass, const std::string &url, Frame &out,
                     const std::string &note = "");

// The diagnostics page: `lines` are drawn top to bottom, a "key: value" a line,
// with the title above them. A line beginning with '!' is drawn in red.
void renderStatusPage(bool portrait, const Font &font, const std::string &title,
                      const std::vector<std::string> &lines, Frame &out);

}  // namespace birdposter
