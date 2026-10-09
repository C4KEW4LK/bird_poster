#include "pages.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace birdposter {

namespace {

// Multiply the canvas by a cream tone, per channel: old parchment's hue,
// #F1E9D2, with extra yellow because the glass reads cool. Against the page's
// paper (242, 237, 226) parchment is red x0.996, green x0.983, blue x0.929;
// level 2 is that with blue down a further 4%, level 3 and 4 the same hue
// stronger. Parchment drops green below red, and on the glass that costs a
// few red dots among the yellow - about one to five, which reads as warm.
//
// Level 1 drops green by less than one step, which is none. The canvas is
// RGB565, so green moves in steps of ~1.6%: at the faint end one step of
// green tips the paper to red before the blue has made any yellow, and the
// faintest cream came out pink.
// The figures are what fall from each channel, measured on flat paper
// through the real dither (vivid, detail and edges at 2):
//   level 1: 3.3% yellow, no red; 2: 15.7 / 2.8; 3: 26.8 / 5.8; 4: 34.9 / 6.5.
void creamPaper(Canvas &canvas, int level) {
  constexpr float kDrop[5][3] = {
      {0, 0, 0}, {0.002f, 0.004f, 0.056f}, {0.004f, 0.017f, 0.111f},
      {0.006f, 0.026f, 0.167f}, {0.008f, 0.034f, 0.222f}};
  const float *d = kDrop[std::clamp(level, 0, 4)];
  const int mul[3] = {int(256 * (1 - d[0])), int(256 * (1 - d[1])), int(256 * (1 - d[2]))};
  for (uint16_t &c : canvas.px) {
    int r, g, b;
    rgb888(c, r, g, b);
    c = rgb565((r * mul[0]) >> 8, (g * mul[1]) >> 8, (b * mul[2]) >> 8);
  }
}

int nowMs() {
  using namespace std::chrono;
  return int(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

float labelScale(LabelSize size) {
  switch (size) {
    case LabelSize::Small: return 0.024f;
    case LabelSize::Large: return 0.042f;
    case LabelSize::XLarge: return 0.055f;
    case LabelSize::Medium: break;
  }
  return 0.032f;
}

float textScale(TextSize size) {
  switch (size) {
    case TextSize::Small: return labelScale(LabelSize::Small);
    case TextSize::Large: return labelScale(LabelSize::Large);
    case TextSize::XLarge: return labelScale(LabelSize::XLarge);
    case TextSize::Huge: return 0.075f;
    case TextSize::Medium: break;
  }
  return labelScale(LabelSize::Medium);
}

namespace {

int subPx(int px, float scale) { return std::max(1, int(std::lround(px * scale))); }
int lineGap(int px) { return px / 8; }

}  // namespace

namespace {

int decodeUtf8(const std::string &s, size_t &i) {
  const unsigned char c = s[i++];
  if (c < 0x80) return c;
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
  int cp = c & (0x3F >> n);
  while (n-- > 0 && i < s.size()) cp = (cp << 6) | (s[i++] & 0x3F);
  return cp;
}

void appendUtf8(std::string &out, int cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Latin, Latin-1 and Latin Extended-A: every letter a common name has been
// seen to carry - Rüppell's, Marañon, the macrons of the New Zealand names.
// Anything further afield is left as it came.
int upper(int cp) {
  if (cp >= 'a' && cp <= 'z') return cp - 32;
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
  if (cp == 0xFF) return 0x178;
  if (cp == 0x131) return 'I';
  if (cp == 0x17F) return 'S';
  // Extended-A pairs capital then small, except two runs that start odd.
  const bool oddRun = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
  if (cp >= 0x100 && cp <= 0x17E && cp != 0x138 && cp != 0x149)
    return (cp % 2 == (oddRun ? 0 : 1)) ? cp - 1 : cp;
  return cp;
}

int lower(int cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
  if (cp == 0x178) return 0xFF;
  if (cp == 0x130) return 'i';
  const bool oddRun = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
  if (cp >= 0x100 && cp <= 0x17E && cp != 0x138 && cp != 0x149)
    return (cp % 2 == (oddRun ? 1 : 0)) ? cp + 1 : cp;
  return cp;
}

}  // namespace

std::string cased(const std::string &name, NameCase how) {
  if (how == NameCase::AsGiven) return name;
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size();) {
    const int cp = decodeUtf8(name, i);
    appendUtf8(out, how == NameCase::Upper ? upper(cp) : lower(cp));
  }
  return out;
}

std::string formatDate(const std::tm &tm, DateStyle style, DateOrder order) {
  // Spelled out here rather than through strftime's %A and %B, which follow
  // the C library's locale and are not the same on the board and the host.
  static const char *const kDays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};
  static const char *const kMonths[] = {"January", "February", "March",     "April",
                                        "May",     "June",     "July",      "August",
                                        "September", "October", "November", "December"};
  const int mon = std::clamp(tm.tm_mon, 0, 11), wday = std::clamp(tm.tm_wday, 0, 6);
  const int year = tm.tm_year + 1900;
  const bool monthFirst = order == DateOrder::MonthFirst;
  const int first = monthFirst ? mon + 1 : tm.tm_mday, second = monthFirst ? tm.tm_mday : mon + 1;
  char buf[48];
  switch (style) {
    case DateStyle::NumericShort:
      std::snprintf(buf, sizeof buf, "%02d/%02d/%02d", first, second, year % 100);
      break;
    case DateStyle::NumericLong:
      std::snprintf(buf, sizeof buf, "%02d/%02d/%04d", first, second, year);
      break;
    case DateStyle::WordsShort:
      std::snprintf(buf, sizeof buf, "%d %.3s %04d", tm.tm_mday, kMonths[mon], year);
      break;
    case DateStyle::WordsLong:
      std::snprintf(buf, sizeof buf, "%d %s %04d", tm.tm_mday, kMonths[mon], year);
      break;
    case DateStyle::WordsFull:
      std::snprintf(buf, sizeof buf, "%s %d %s %04d", kDays[wday], tm.tm_mday, kMonths[mon], year);
      break;
  }
  return buf;
}

namespace {

// Each "{{name}}" in `text`, lower-cased with its spaces dropped: `each` is
// told where it starts, one past where it ends, and the name.
template <typename Each>
size_t eachName(const std::string &text, Each each) {
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t open = text.find("{{", pos);
    const size_t close = open == std::string::npos ? open : text.find("}}", open + 2);
    if (close == std::string::npos) break;
    std::string name;
    for (char c : text.substr(open + 2, close - open - 2))
      if (c != ' ') name += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (!each(pos, open, close + 2, name)) return std::string::npos;
    pos = close + 2;
  }
  return pos;
}

}  // namespace

bool textUses(const std::string &text, const std::string &name, bool prefix) {
  bool found = false;
  eachName(text, [&](size_t, size_t, size_t, const std::string &n) {
    found = found || n == name || (prefix && n.compare(0, name.size(), name) == 0);
    return !found;
  });
  return found;
}

std::string formatTime(const std::tm &tm, bool clock12h) {
  char buf[16];
  if (clock12h) {
    const int h = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    std::snprintf(buf, sizeof buf, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
  } else {
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  }
  return buf;
}

std::string formatHour(const std::tm &tm, bool clock12h) {
  if (!clock12h) return std::to_string(tm.tm_hour);
  return std::to_string(tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12) + (tm.tm_hour < 12 ? " am" : " pm");
}

std::string expandText(const std::string &text, const std::tm *tm, const TextPrefs &prefs,
                       const TextLookup &lookup) {
  const DateOrder order = prefs.dateOrder;
  static const char *const kDays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};
  static const char *const kMonths[] = {"January", "February", "March",     "April",
                                        "May",     "June",     "July",      "August",
                                        "September", "October", "November", "December"};
  // What a name stands for, or false for a name that is not one.
  const auto value = [order, &prefs](const std::string &name, const std::tm &t, std::string &out) {
    const std::tm *tm = &t;
    const int mon = std::clamp(tm->tm_mon, 0, 11), wday = std::clamp(tm->tm_wday, 0, 6);
    char buf[32];
    if (name == "date.long") out = formatDate(*tm, DateStyle::WordsLong, order);
    else if (name == "date.short") out = formatDate(*tm, DateStyle::NumericShort, order);
    else if (name == "date.numeric") out = formatDate(*tm, DateStyle::NumericLong, order);
    else if (name == "date.medium") out = formatDate(*tm, DateStyle::WordsShort, order);
    else if (name == "date.full") out = formatDate(*tm, DateStyle::WordsFull, order);
    else if (name == "time") out = formatTime(*tm, prefs.clock12h);
    else if (name == "time.24h") out = formatTime(*tm, false);
    else if (name == "time.12h") out = formatTime(*tm, true);
    else if (name == "hour.24h") out = formatHour(*tm, false);
    else if (name == "hour.12h") out = formatHour(*tm, true);
    else if (name == "weekday") out = kDays[wday];
    else if (name == "weekday.short") out = std::string(kDays[wday], 3);
    else if (name == "day") out = std::to_string(tm->tm_mday);
    else if (name == "month") out = kMonths[mon];
    else if (name == "month.short") out = std::string(kMonths[mon], 3);
    else if (name == "month.number") {
      std::snprintf(buf, sizeof buf, "%02d", mon + 1);
      out = buf;
    } else if (name == "year") out = std::to_string(tm->tm_year + 1900);
    else return false;
    return true;
  };
  static const std::tm kAny{};  // stands in for the clock to tell a name from not one
  std::string out;
  const size_t end = eachName(text, [&](size_t pos, size_t open, size_t close,
                                        const std::string &name) {
    out.append(text, pos, open - pos);
    std::string filled;
    if (value(name, tm ? *tm : kAny, filled)) {
      if (!tm) return false;
    } else {
      const TextValue v = lookup ? lookup(name, filled) : TextValue::Unknown;
      if (v == TextValue::Missing) return false;
      if (v == TextValue::Unknown) filled = text.substr(open, close - open);
    }
    out += filled;
    return true;
  });
  if (end == std::string::npos) return "";
  if (end < text.size()) out.append(text, end, std::string::npos);
  return out;
}

LabelBox nameBox(const Font &firstFont, const Font &secondFont, const std::string &first,
                 const std::string &second, int px, float subScale) {
  int a1 = 0, d1 = 0, a2 = 0, d2 = 0;
  if (second.empty()) return LabelBox{firstFont.measure(first, px, &a1, &d1), a1 + d1};
  if (first.empty()) return LabelBox{secondFont.measure(second, px, &a1, &d1), a1 + d1};
  const int w1 = firstFont.measure(first, px, &a1, &d1);
  const int w2 = secondFont.measure(second, subPx(px, subScale), &a2, &d2);
  return LabelBox{std::max(w1, w2), a1 + d1 + lineGap(px) + a2 + d2};
}

void drawName(Frame &frame, const Font &firstFont, const Font &secondFont,
              const std::string &first, const std::string &second, int boxX, int boxY, int boxW,
              int boxH, int px, float subScale) {
  if (second.empty()) {
    drawCentred(frame, firstFont, first, boxX, boxY, boxW, boxH, px, kBlack);
    return;
  }
  if (first.empty()) {
    drawCentred(frame, secondFont, second, boxX, boxY, boxW, boxH, px, kBlack);
    return;
  }
  int a1 = 0, d1 = 0, a2 = 0, d2 = 0;
  const int w1 = firstFont.measure(first, px, &a1, &d1);
  const int sub = subPx(px, subScale);
  const int w2 = secondFont.measure(second, sub, &a2, &d2);
  const int total = a1 + d1 + lineGap(px) + a2 + d2;
  const int top = boxY + (boxH - total) / 2;
  firstFont.draw(frame, first, boxX + (boxW - w1) / 2, top + a1, px, kBlack);
  secondFont.draw(frame, second, boxX + (boxW - w2) / 2, top + a1 + d1 + lineGap(px) + a2, sub,
                  kBlack);
}

bool renderBirdPage(const Plates &plates, const std::vector<int> &plateIndices,
                    const BirdPageSettings &settings, const Font &font, Frame &out,
                    BirdPageReport *report) {
  const auto [width, height] = pageSize(settings.portrait);
  const auto progress = [&settings](const char *what) {
    if (settings.progress) settings.progress(what);
  };
  const auto commonName = [&settings](size_t i) -> const std::string & {
    static const std::string none;
    return i < settings.commonNames.size() ? settings.commonNames[i] : none;
  };
  const bool names = settings.names != NameStyle::None;
  const float subScale = std::clamp(settings.subNamePercent, 30, 100) / 100.0f;
  // The common name's face, where one was given and loaded; else the label's.
  const Font &nameFace =
      settings.commonFont && settings.commonFont->ok() ? *settings.commonFont : font;
  // The face a bird's first line is set in: the common name's, unless it
  // lacks a character - the frame's is capitals only, so a lower-case name,
  // or a scientific name standing in for a missing common one, goes in the
  // label face instead of dropping letters.
  const auto firstFace = [&](const std::string &text) -> const Font & {
    return nameFace.covers(text) ? nameFace : font;
  };
  // The two lines a bird's label carries under this style, either possibly
  // empty. A bird with no common name shows its scientific one instead of
  // nothing.
  const auto styled = [&](size_t i, const std::string &scientific) {
    const std::string common = cased(commonName(i), settings.commonCase);
    switch (settings.names) {
      case NameStyle::Both: return std::make_pair(common, scientific);
      case NameStyle::Scientific: return std::make_pair(std::string(), scientific);
      case NameStyle::Common: return std::make_pair(common.empty() ? scientific : common, std::string());
      case NameStyle::None: break;
    }
    return std::make_pair(std::string(), std::string());
  };
  // With its suffix, if it has one, after whichever line comes first.
  const auto lines = [&](size_t i, const std::string &scientific) {
    auto pair = styled(i, scientific);
    if (i < settings.nameSuffixes.size() && !settings.nameSuffixes[i].empty()) {
      std::string &line = pair.first.empty() ? pair.second : pair.first;
      if (!line.empty()) line += " " + settings.nameSuffixes[i];
    }
    return pair;
  };

  // What the packer wants of each bird, all of it out of the pack's index and
  // the silhouette stream - the pixels are not touched until a bird has a place.
  std::vector<Mask> sources;
  std::vector<bool> flips;
  std::vector<LabelBox> labels;
  // The luma planes the silhouette pass decoded, for the draw to take up;
  // empty for a bird past the budget.
  std::vector<std::vector<uint8_t>> kept(plateIndices.size());
  size_t keptBytes = 0;
  int lumaKept = 0;
  progress("loading silhouettes");
  fastStats = FastStats{};
  const int tMasks = nowMs();
  for (size_t i = 0; i < plateIndices.size(); ++i) {
    const int idx = plateIndices[i];
    const PlateEntry &e = plates.entry(size_t(idx));
    Mask m;
    const size_t keepBytes = (size_t(e.w) * e.h + 1) / 2;
    const bool keep = keptBytes + keepBytes <= settings.keepLumaBytes;
    if (!plates.loadMask(size_t(idx), m, keep ? &kept[i] : nullptr)) return false;
    if (keep) keptBytes += keepBytes, ++lumaKept;
    sources.push_back(std::move(m));
    flips.push_back(flipFor(e.flip, e.name.c_str(), settings.variant));
    if (names) {
      const auto [first, second] = lines(i, e.name);
      // The baked box is the scientific name alone; anything else is
      // measured here, in the device's own font.
      const bool baked = first.empty() && second == e.name;
      labels.push_back(baked || !font.ok() ? e.label
                                           : nameBox(firstFace(first), font, first, second, kLabelRefPx,
                                                     subScale));
    }
  }
  if (sources.empty()) return false;

  // Pack inside the margin but size off the whole page: see the host harness.
  // kMargin is the packer's own, the same on every side; the owner's is added
  // to it a side at a time, and clamped so a mistyped number still leaves a
  // page to draw on.
  const int packMargin = int(std::lround(std::min(width, height) * kMargin));
  const auto border = [packMargin](int given, int axis) {
    return packMargin + std::clamp(given, 0, axis / 4);
  };
  const int mLeft = border(settings.marginLeft, width);
  const int mRight = border(settings.marginRight, width);
  const int mTop = border(settings.marginTop, height);
  const int mBottom = border(settings.marginBottom, height);
  const int boxW = width - mLeft - mRight, boxH = height - mTop - mBottom;
  const int namePx = std::max(
      kMinLabelPx, int(std::lround(std::min(width, height) * labelScale(settings.labelSize))));

  // The owner's lines each take a band along their
  // edge, inset from the glass's own edge, and the birds are packed into what
  // is left so none sits under them. Each end of each edge is a slot; what
  // lands in one slot is written along it in turn, a space apart.
  struct Run {
    std::string text;
    int px;
    const Font *face;
    int w = 0, ascent = 0, descent = 0;
  };
  struct Slot {
    std::vector<Run> runs;
    int w = 0;
  };
  Slot slots[2][3];  // [TextEdge][TextAlign]
  const int inset = std::max(4, namePx / 2);
  const auto put = [&](const std::string &text, TextEdge edge, TextAlign align, int px,
                       const Font *face = nullptr) {
    if (text.empty() || !font.ok()) return;
    std::string set = text;
    if (face && face->ok() && face != &font) {
      set = cased(text, NameCase::Upper);
      if (!face->covers(set)) set = text, face = nullptr;
    } else {
      face = nullptr;
    }
    slots[int(edge)][int(align)].runs.push_back(Run{set, px, face ? face : &font});
  };
  const auto linePx = [&](TextSize size) {
    return std::max(kMinLabelPx, int(std::lround(std::min(width, height) *
                                                 textScale(size))));
  };
  put(settings.topText, TextEdge::Top, settings.topAlign, linePx(settings.topSize), settings.topFont);
  put(settings.bottomText, TextEdge::Bottom, settings.bottomAlign, linePx(settings.bottomSize),
      settings.bottomFont);
  const int textW = boxW - 2 * inset;
  int band[2] = {0, 0}, edgeAscent[2] = {0, 0}, edgeDescent[2] = {0, 0};
  for (int e = 0; e < 2; ++e) {
    for (Slot &slot : slots[e]) {
      if (slot.runs.empty()) continue;
      // Measured at its size, and shrunk together to fit across the page if
      // it does not: an owner's line can be as long as they like.
      const auto measure = [&] {
        slot.w = 0;
        for (Run &r : slot.runs) {
          r.w = r.face->measure(r.text, r.px, &r.ascent, &r.descent);
          slot.w += r.w + (slot.w ? namePx / 2 : 0);
        }
      };
      measure();
      if (slot.w > textW && slot.w > 0) {
        const float scale = float(textW) / float(slot.w);
        for (Run &r : slot.runs) r.px = std::max(kMinLabelPx, int(r.px * scale));
        measure();
      }
      for (const Run &r : slot.runs) {
        edgeAscent[e] = std::max(edgeAscent[e], r.ascent);
        edgeDescent[e] = std::max(edgeDescent[e], r.descent);
      }
    }
    if (edgeAscent[e] + edgeDescent[e] > 0)
      band[e] = inset + edgeAscent[e] + edgeDescent[e] + namePx / 3;
  }
  const int packH = boxH - band[0] - band[1];
  const int originX = mLeft, originY = mTop + band[0];

  std::vector<Placement> placed;
  int usedPx = 0;
  PackPlan plan = planFor(settings.packStyle);
  progress("packing the page");
  const int t0 = nowMs();
  int attempts = 0;
  packCounters = PackCounters{};
  // At a reduced resolution (packScale), the packer works on silhouettes and
  // a page 1/s the size, and its placements are scaled back up after. Names
  // stay in page pixels throughout (see setPackScale).
  const int ps = std::clamp(settings.packScale, 1, 8);
  if (ps > 1)
    for (Mask &m : sources) m = m.reduced(ps);
  setPackScale(ps);
  struct ScaleBack {
    ~ScaleBack() { setPackScale(1); }
  } scaleBack;
  bool fellBack = false;
  if (!layout(sources, flips, labels, namePx, width / ps, height / ps, boxW / ps, packH / ps, placed,
              &usedPx, settings.variant, plan.pack, &attempts)) {
    // Grid, scatter and hero give every bird a place of a size the page
    // decides; a set whose names cannot shrink under kMinLabelPx can be
    // wider than any of those places at every scale, and then no page is
    // drawn at all. The spiral fits names around birds instead: lay the page
    // out that way rather than leave the glass as it was.
    if (settings.packStyle == PackStyle::Classic) return false;
    plan = planFor(PackStyle::Classic);
    fellBack = true;
    int more = 0;
    if (!layout(sources, flips, labels, namePx, width / ps, height / ps, boxW / ps, packH / ps,
                placed, &usedPx, settings.variant, plan.pack, &more))
      return false;
    attempts += more;
  }
  const int tGrow = nowMs();
  // Then let each bird take the room beside it.
  if (settings.grow) {
    progress("growing into the gaps");
    // The hero is the first bird given, and keeps the pocket under its name
    // closed while it grows, as the layout did.
    grow(sources, flips, labels, usedPx, boxW / ps, packH / ps, placed, plan.growMax,
         plan.growNudge, plan.growRounds, plan.growStep, plan.pack.hero ? 0 : -1);
  }
  setPackScale(1);
  if (ps > 1)
    for (Placement &p : placed) {
      p.dim *= ps;
      p.x *= ps;
      p.y *= ps;
      p.labelX *= ps;
      p.labelY *= ps;
      p.labelW *= ps;
      p.labelH *= ps;
    }
  const int t1 = nowMs();

  // The silhouettes are done with; the canvas wants the memory more.
  sources.clear();
  sources.shrink_to_fit();

  // A bird's drawn size, and whether it is drawn well above its baked size -
  // where spriteOverride is asked for a larger source.
  struct Drawn {
    int w, h;
    bool larger;
  };
  const auto drawn = [&](const Placement &p) {
    const PlateEntry &e = plates.entry(size_t(plateIndices[size_t(p.index)]));
    // dim is the silhouette's longest side at this size, and the sprite is
    // the silhouette, so it lands exactly where the packer put the mask.
    const float s = float(p.dim) / float(std::max(e.w, e.h));
    const int dw = std::max(1, int(std::lround(e.w * s)));
    const int dh = std::max(1, int(std::lround(e.h * s)));
    return Drawn{dw, dh, dw * 4 > e.w * 5 || dh * 4 > e.h * 5};
  };
  if (settings.afterLayout) {
    std::vector<size_t> larger;
    for (const Placement &p : placed)
      if (drawn(p).larger) larger.push_back(size_t(p.index));
    settings.afterLayout(larger);
  }

  Canvas canvas;
  const size_t pagePx = size_t(width) * height;
  const bool reserved = settings.pageMemory && settings.pageMemoryPx >= pagePx;
  if (reserved) canvas.px.adopt(settings.pageMemory, settings.pageMemoryPx);
  const uint8_t *paper = plates.paper();
  canvas.reset(width, height, rgb565(paper[0], paper[1], paper[2]));
  progress("drawing the birds");
  int decodeMs = 0, resampleMs = 0, overrideMs = 0;
  for (const Placement &p : placed) {
    const int idx = plateIndices[size_t(p.index)];
    const Drawn d = drawn(p);
    const int dw = d.w, dh = d.h;
    SpriteImage sprite;
    const bool larger = d.larger;
    const int tOverride = nowMs();
    const bool overridden =
        larger && settings.spriteOverride && settings.spriteOverride(size_t(p.index), sprite);
    const int tDecode = nowMs();
    overrideMs += tDecode - tOverride;
    if (!overridden && !plates.loadSprite(size_t(idx), sprite, &kept[size_t(p.index)])) return false;
    kept[size_t(p.index)] = std::vector<uint8_t>();  // drawn: give its memory back
    const int tResample = nowMs();
    decodeMs += tResample - tDecode;
    const bool enlarged = dw * 2 > sprite.w * 3 || dh * 2 > sprite.h * 3;
    drawSprite(canvas, sprite, originX + p.x, originY + p.y, dw, dh, flips[size_t(p.index)],
               enlarged ? settings.resample : Resample::Bilinear);
    resampleMs += nowMs() - tResample;
  }
  const int t2 = nowMs();

  if (settings.cream > 0) creamPaper(canvas, settings.cream);
  if (settings.beforeDither) settings.beforeDither(canvas);
  progress("dithering to six inks");
  if (reserved) {
    // The frame goes into the canvas's own memory, a byte a pixel over its two.
    out.w = width;
    out.h = height;
    out.px.adopt(reinterpret_cast<uint8_t *>(settings.pageMemory), settings.pageMemoryPx * 2, pagePx);
  }
  DitherCounters ditherCounters;
  dither(canvas, out, settings.vivid, settings.sharpen, settings.edges, settings.jitter,
         &ditherCounters);
  canvas.px.release();
  const int t3 = nowMs();

  if (names) {
    progress("writing the names");
    for (const Placement &p : placed) {
      if (!p.hasLabel) continue;
      const PlateEntry &e = plates.entry(size_t(plateIndices[size_t(p.index)]));
      const auto [first, second] = lines(size_t(p.index), e.name);
      // The size this bird's box was reserved at, which is the set's except
      // for a hero's, set larger - see Placement::labelPx.
      const int px = p.labelPx > 0 ? p.labelPx : usedPx;
      drawName(out, firstFace(first), font, first, second, originX + p.labelX, originY + p.labelY,
               p.labelW, p.labelH, px, subScale);
    }
  }

  const int left = mLeft + inset, right = width - mRight - inset;
  for (int e = 0; e < 2; ++e) {
    // Everything on an edge shares one baseline.
    const int baseline = e == int(TextEdge::Top) ? mTop + inset + edgeAscent[e]
                                                 : height - mBottom - inset - edgeDescent[e];
    for (int a = 0; a < 3; ++a) {
      const Slot &slot = slots[e][a];
      if (slot.runs.empty()) continue;
      progress("writing the page's text");
      int x = a == int(TextAlign::Left)     ? left
              : a == int(TextAlign::Centre) ? (left + right - slot.w) / 2
                                            : right - slot.w;
      for (const Run &r : slot.runs) {
        r.face->draw(out, r.text, x, baseline, r.px, kBlack);
        x += r.w + namePx / 2;
      }
    }
  }

  if (report) {
    report->placed = int(placed.size());
    report->labelPx = names ? usedPx : 0;
    report->packMs = t1 - t0;
    report->drawMs = t2 - t1;
    report->ditherMs = t3 - t2;
    report->masksMs = t0 - tMasks;
    report->layoutMs = tGrow - t0;
    report->growMs = t1 - tGrow;
    report->decodeMs = decodeMs;
    report->resampleMs = resampleMs;
    report->overrideMs = overrideMs;
    report->textMs = nowMs() - t3;
    report->attempts = attempts;
    report->pack = packCounters;
    report->placements = placed;
    report->fellBack = fellBack;
    report->birdsTop = originY;
    report->birdsBottom = originY + packH;
    report->lumaKept = lumaKept;
    report->dither = ditherCounters;
    report->fast = fastStats;
    std::vector<int> dims;
    for (const Placement &p : placed) dims.push_back(p.dim);
    std::sort(dims.begin(), dims.end());
    report->medianDim = dims[dims.size() / 2];
  }
  return true;
}

namespace {

// A thin rule of ink, the one decoration the plain pages get.
void rule(Frame &f, int x, int y, int w) { f.fillRect(x, y, w, 3, kBlack); }

}  // namespace

void renderSetupPage(bool portrait, const Font &font, const std::string &ssid,
                     const std::string &pass, const std::string &url, Frame &out,
                     const std::string &note) {
  const auto [width, height] = pageSize(portrait);
  out.reset(width, height, kWhite);
  const int shortSide = std::min(width, height);
  const int margin = shortSide / 12;
  const int big = shortSide / 14, body = shortSide / 24, small = shortSide / 32;

  int ascent = 0, descent = 0;
  font.measure("x", big, &ascent, &descent);
  int y = margin + ascent;
  font.draw(out, "Bird poster", margin, y, big, kBlack);
  y += descent + body / 2;
  rule(out, margin, y, width - 2 * margin);
  y += body;

  font.measure("x", body, &ascent, &descent);
  const int line = ascent + descent + body / 3;
  y += ascent;
  font.draw(out, note.empty() ? std::string("Not on a WiFi network yet.") : note, margin, y,
            body, note.empty() ? kBlack : kRed);
  y += line;
  font.draw(out, "Scan the code to join the frame's own network, then set it up:", margin, y,
            body, kBlack);
  y += line + body / 2;

  // The footer sits on the page's bottom edge; everything else has the room
  // between here and it. The code takes as much of that as it can - a phone
  // reads a module of 6 px on this glass from arm's length, and more is only
  // easier - and the details go beside it, key and value on a line.
  const int footerPx = small;
  int fAsc = 0, fDesc = 0;
  font.measure("x", footerPx, &fAsc, &fDesc);
  const int footerTop = height - margin - fAsc - fDesc;
  const std::string payload = wifiQrPayload(ssid, pass);
  const int room = std::min((width - 2 * margin) * 2 / 5, footerTop - body - y);
  int module = 16;
  while (module > 4 && qrSize(payload, module) > room) --module;
  const int side = drawQr(out, payload, margin, y, module);

  const int tx = margin + side + body;
  const int keyW = font.measure("Password", small) + body;
  int ty = y + ascent + body / 2;
  const auto row = [&](const char *key, const std::string &value, Ink ink) {
    font.draw(out, key, tx, ty, small, kBlack);
    font.draw(out, value, tx + keyW, ty, body, ink);
    ty += line + body / 3;
  };
  row("Network", ssid, kBlack);
  row("Password", pass.empty() ? std::string("(none)") : pass, kBlack);
  row("Then open", url, kBlue);
  font.draw(out, "Most phones open the page on their own once joined.", tx, ty, small, kBlack);

  font.draw(out, "Key 1 keeps WiFi on for setup.  Key 2 shows this frame's status.", margin,
            footerTop + fAsc, footerPx, kBlack);
}

void renderStatusPage(bool portrait, const Font &font, const std::string &title,
                      const std::vector<std::string> &lines, Frame &out) {
  const auto [width, height] = pageSize(portrait);
  out.reset(width, height, kWhite);
  const int shortSide = std::min(width, height);
  const int margin = shortSide / 12;
  const int big = shortSide / 14, body = shortSide / 30;

  int ascent = 0, descent = 0;
  font.measure("x", big, &ascent, &descent);
  int y = margin + ascent;
  font.draw(out, title, margin, y, big, kBlack);
  y += descent + body / 2;
  rule(out, margin, y, width - 2 * margin);
  y += body;

  font.measure("x", body, &ascent, &descent);
  const int line = ascent + descent + body / 4;
  y += ascent;
  // Two columns once the first runs off the page - a landscape page has the
  // width for it and a status page can have a lot to say.
  int x = margin;
  const int colW = (width - 2 * margin) / 2;
  const int bottom = height - margin;
  for (const std::string &raw : lines) {
    if (y + descent > bottom) {
      if (x != margin) break;
      x += colW;
      y = margin + big + body * 2 + ascent;
    }
    const bool warn = !raw.empty() && raw[0] == '!';
    const std::string text = warn ? raw.substr(1) : raw;
    // "key: value" - the key in the small face, the value after it.
    const size_t colon = text.find(": ");
    if (colon == std::string::npos) {
      font.draw(out, text, x, y, body, warn ? kRed : kBlack);
    } else {
      const std::string key = text.substr(0, colon + 1), value = text.substr(colon + 2);
      const int keyPx = body * 3 / 4;
      font.draw(out, key, x, y, keyPx, kBlack);
      const int kw = font.measure(key, keyPx) + body / 2;
      font.draw(out, value, x + kw, y, body, warn ? kRed : kBlack);
    }
    y += line;
  }
}

}  // namespace birdposter
