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
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "plates.h"
#include "render.h"

namespace birdposter {

// page.LABEL_SIZES: a name as a fraction of the page's short side.
enum class LabelSize { Small, Medium, Large, XLarge };
float labelScale(LabelSize size);

// The size of one of the owner's lines of text: the name sizes, and one
// larger for a heading. 0 is not used: it was "the names' size" once, and a
// setting saved then is read as that size (see loadSettings).
enum class TextSize : uint8_t { Small = 1, Medium, Large, XLarge, Huge };
// As a fraction of the page's short side, like labelScale.
float textScale(TextSize size);

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

// How a date is written, for the page text's {{date...}} names. The numeric
// styles follow DateOrder; the ones in words are always day first.
enum class DateStyle : uint8_t {
  NumericShort = 0,  // 26/09/26, or 09/26/26 month first
  NumericLong = 1,   // 26/09/2026, or 09/26/2026 month first
  WordsShort = 2,    // 26 Sep 2026
  WordsLong = 3,     // 26 September 2026
  WordsFull = 4,     // Saturday 26 September 2026
};
enum class DateOrder : uint8_t { DayFirst = 0, MonthFirst = 1 };  // UK/AU, US
// Which edge of the page a line of text sits on, and which end of it.
enum class TextEdge : uint8_t { Top = 0, Bottom = 1 };
enum class TextAlign : uint8_t { Left = 0, Centre = 1, Right = 2 };
std::string formatDate(const std::tm &tm, DateStyle style,
                       DateOrder order = DateOrder::DayFirst);
// A time of day: 14:05, or 2:05 pm on a 12-hour clock.
std::string formatTime(const std::tm &tm, bool clock12h = false);
// The hour alone: 14, or 2 pm.
std::string formatHour(const std::tm &tm, bool clock12h = false);

// How the owner likes dates and times written, for the page text.
struct TextPrefs {
  DateOrder dateOrder = DateOrder::DayFirst;  // for the numeric dates
  bool clock12h = false;                      // 2:05 pm rather than 14:05
};

// The owner's own line of text, with "{{name}}" filled in from the clock:
//
//   date.long         26 September 2026     date.short    26/09/26
//   date.medium       26 Sep 2026           date.numeric  26/09/2026
//   date.full         Saturday 26 September 2026
//   time              14:05, or 2:05 pm on a 12-hour clock
//   time.24h          14:05 whatever the clock  time.12h  2:05 pm whatever the clock
//   hour.24h          14                        hour.12h  2 pm
//   weekday           Saturday              weekday.short Sat
//   day  26   month  September   month.short  Sep   month.number  09   year  2026
//
// and from `lookup` for any other name - what the frame knows of the page and
// itself, which the renderer does not. The numeric dates and the time follow
// `prefs`.
// Names are not case sensitive and may have spaces inside the braces; one
// nobody knows is left as written. `tm` null means the clock is not set.
//
// A line that asks for something there is none of - the clock not set, or a
// name `lookup` answers Missing - comes back empty, rather than with a hole:
// a page dated 1970, or "Battery: " and nothing, is worse than no line.
enum class TextValue : uint8_t { Unknown, Filled, Missing };
using TextLookup = std::function<TextValue(const std::string &name, std::string &out)>;
std::string expandText(const std::string &text, const std::tm *tm,
                       const TextPrefs &prefs = {}, const TextLookup &lookup = nullptr);

// Whether `text` asks for "{{name}}", as expandText would read it; or, with
// `prefix`, for any name that starts with `name` ("weather.").
bool textUses(const std::string &text, const std::string &name, bool prefix = false);

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
  // A border the page draws nothing in, in page pixels, one side at a time.
  // The glass is the whole page, so by default a bird runs off its edge and a
  // name can end up against it; a frame in a mount wants everything held back
  // behind the rebate, and a rebate is rarely even on all four sides. The
  // birds are packed into what is left, so they fill it rather than float in
  // it. Clamped per side to a quarter of that axis, so a mistyped number
  // cannot leave a page with no room on it.
  int marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
  bool grow = true;  // let birds grow into the gaps after the pack (off: the packed layout alone)
  PackStyle packStyle = PackStyle::Classic;  // how the page is arranged; see packer.h
  // The owner's own lines, already expanded (see expandText), one along each
  // edge at their own sizes, with the birds packed into what is left.
  // Empty means none.
  std::string topText, bottomText;
  TextAlign topAlign = TextAlign::Centre, bottomAlign = TextAlign::Centre;
  // The face each of the owner's lines is set in. Null means the label font;
  // the common name's face is capitals only, so a line set in it is put in
  // capitals, and one it still lacks a character for goes in the label font.
  const Font *topFont = nullptr, *bottomFont = nullptr;
  // How large each line is set.
  TextSize topSize = TextSize::Medium, bottomSize = TextSize::Medium;
  // Parallel to the plate indices; shorter, or an empty string, means none.
  std::vector<std::string> commonNames;
  // Also parallel: written after the first line of a bird's name, a space
  // apart - BirdNET-Go's confidence, "(87%)". Shorter, or empty, means none.
  std::vector<std::string> nameSuffixes;
  // The face the common name is set in. Null, or not loaded, means the label
  // font - the scientific name is always in that.
  const Font *commonFont = nullptr;
  // A sprite for a page position from somewhere other than the pack - the
  // full-size plates the frame fetches over the network, or a host harness
  // experiment - returning false to fall back to the pack's. Asked only for a
  // bird drawn more than 1.25x its baked size, where a larger source shows;
  // drawn on the pack's own silhouette and layout, scaled to the same box.
  std::function<bool(size_t index, SpriteImage &out)> spriteOverride;
  // Told, once the layout is settled and before anything is drawn, which page
  // positions will be asked of spriteOverride - the birds drawn more than
  // 1.25x their baked size. The frame fetches their full-size plates here and
  // then turns its radio off, so the long render runs without it. May be empty.
  std::function<void(const std::vector<size_t> &larger)> afterLayout;
  // Host harness only: shown the canvas as the dither is about to see it -
  // the birds drawn and scaled, the paper tinted - for looking at.
  std::function<void(const Canvas &)> beforeDither;
  // Told what the render is doing as it goes, for a progress display. May be
  // empty.
  std::function<void(const char *)> progress;
  // Memory the render may spend keeping each bird's luma plane from the
  // silhouette pass, so drawing it does not decode it a second time - half a
  // byte a plate pixel, held from the packing until the bird is drawn, when
  // the canvas is also up. Birds past the budget are decoded twice, as before.
  size_t keepLumaBytes = SIZE_MAX;
  // Memory set aside for the page, `pageMemoryPx` RGB565 pixels at
  // `pageMemory`, or null to allocate as it goes. The canvas is drawn in it
  // and the frame dithered into the same block in place (see `dither`), so
  // `out` comes back holding this memory, not a copy - it is the page until
  // the memory is next used. Needs width x height of the page.
  uint16_t *pageMemory = nullptr;
  size_t pageMemoryPx = 0;
  // The resolution the packer works at, as a divisor of the page's: 1 full,
  // 2 half, 4 quarter. Coarser is faster and packs a little looser; birds
  // never overlap more than at full (see Mask::reduced, setPackScale).
  int packScale = 1;
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
  int packMs = 0;        // layoutMs + growMs
  int drawMs = 0;        // decodeMs + resampleMs + overrideMs
  int ditherMs = 0;
  int medianDim = 0;
  // The same time, finer. Each is wall time, so a slow filesystem shows in
  // whichever stage was reading it.
  int masksMs = 0;       // the silhouettes: read and decode the luma plane, measure the names
  int layoutMs = 0;      // the scale search
  int growMs = 0;        // each bird into the room beside it
  int decodeMs = 0;      // the placed birds' planes, read and decoded again for their pixels
  int resampleMs = 0;    // scaling them onto the canvas
  int overrideMs = 0;    // in `spriteOverride`: the caller's own sprites (the web's)
  int textMs = 0;        // names, date and note, after the dither
  int attempts = 0;      // scales the layout tried, each a whole packing of the set
  PackCounters pack;     // inside layout and grow: what their time went on
  int lumaKept = 0;      // birds whose luma plane was kept, not decoded twice
  DitherCounters dither; // inside ditherMs
  FastStats fast;        // fast-memory requests in the render, granted or not
  std::vector<Placement> placements;  // where each bird went, in page pixels
  bool fellBack = false; // the pack style could not fit the set, and it was laid out as classic
  // The rows the birds were packed into, [birdsTop, birdsBottom): the page
  // inside its margins less the bands the date and the text took. Placements
  // are from birdsTop down.
  int birdsTop = 0, birdsBottom = 0;
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
