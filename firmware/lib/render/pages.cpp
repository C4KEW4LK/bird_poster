#include "pages.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace birdposter {

namespace {

// Multiply the canvas by a cream tone, per channel. At `level` 4 the paper
// comes out near a light buff; below that, proportionally less. Red holds,
// green falls a little and blue most, which is what makes it warm.
void creamPaper(Canvas &canvas, int level) {
  const float t = std::clamp(level, 0, 4) / 4.0f;
  const int mul[3] = {int(256 * (1 - 0.01f * t)), int(256 * (1 - 0.05f * t)),
                      int(256 * (1 - 0.17f * t))};
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
  const auto lines = [&](size_t i, const std::string &scientific) {
    const std::string common = cased(commonName(i), settings.commonCase);
    switch (settings.names) {
      case NameStyle::Both: return std::make_pair(common, scientific);
      case NameStyle::Scientific: return std::make_pair(std::string(), scientific);
      case NameStyle::Common: return std::make_pair(common.empty() ? scientific : common, std::string());
      case NameStyle::None: break;
    }
    return std::make_pair(std::string(), std::string());
  };

  // What the packer wants of each bird, all of it out of the pack's index and
  // the silhouette stream - the pixels are not touched until a bird has a place.
  std::vector<Mask> sources;
  std::vector<bool> flips;
  std::vector<LabelBox> labels;
  progress("loading silhouettes");
  for (size_t i = 0; i < plateIndices.size(); ++i) {
    const int idx = plateIndices[i];
    const PlateEntry &e = plates.entry(size_t(idx));
    Mask m;
    if (!plates.loadMask(size_t(idx), m)) return false;
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
  const int margin = int(std::lround(std::min(width, height) * kMargin));
  const int boxW = width - 2 * margin, boxH = height - 2 * margin;
  const int namePx = std::max(
      kMinLabelPx, int(std::lround(std::min(width, height) * labelScale(settings.labelSize))));

  // The date takes a band along its edge, inset from the glass's own edge,
  // and the birds are packed into the rest so none sits under it.
  const bool dated = !settings.date.empty() && font.ok();
  const bool noted = !settings.note.empty() && font.ok();
  const bool dateTop = settings.dateEdge == DateEdge::Top;
  int dateAscent = 0, dateDescent = 0, dateW = 0, band = 0;
  const int dateInset = std::max(4, namePx / 2);
  const int notePx = std::max(kMinLabelPx, namePx * 3 / 4);
  int noteAscent = 0, noteDescent = 0, noteW = 0;
  if (dated) dateW = font.measure(settings.date, namePx, &dateAscent, &dateDescent);
  if (noted) noteW = font.measure(settings.note, notePx, &noteAscent, &noteDescent);
  if (dated || noted) {
    const int line = std::max(dateAscent + dateDescent, noteAscent + noteDescent);
    band = dateInset + line + namePx / 3;
  }
  const int packH = boxH - band;
  const int originY = margin + (dateTop ? band : 0);

  std::vector<Placement> placed;
  int usedPx = 0;
  const PackPlan plan = planFor(settings.packStyle);
  progress("packing the page");
  const int t0 = nowMs();
  if (!layout(sources, flips, labels, namePx, width, height, boxW, packH, placed, &usedPx,
              settings.variant, plan.pack))
    return false;
  // Then let each bird take the room beside it.
  if (settings.grow) {
    progress("growing into the gaps");
    grow(sources, flips, labels, usedPx, boxW, packH, placed, plan.growMax, plan.growNudge,
         plan.growRounds, plan.growStep);
  }
  const int t1 = nowMs();

  // The silhouettes are done with; the canvas wants the memory more.
  sources.clear();
  sources.shrink_to_fit();

  Canvas canvas;
  const uint8_t *paper = plates.paper();
  canvas.reset(width, height, rgb565(paper[0], paper[1], paper[2]));
  progress("drawing the birds");
  for (const Placement &p : placed) {
    const int idx = plateIndices[size_t(p.index)];
    const PlateEntry &e = plates.entry(size_t(idx));
    // dim is the silhouette's longest side at this size, and the sprite is
    // the silhouette, so it lands exactly where the packer put the mask.
    const float s = float(p.dim) / float(std::max(e.w, e.h));
    const int dw = std::max(1, int(std::lround(e.w * s)));
    const int dh = std::max(1, int(std::lround(e.h * s)));
    SpriteImage sprite;
    const bool larger = dw * 4 > e.w * 5 || dh * 4 > e.h * 5;
    const bool overridden =
        larger && settings.spriteOverride && settings.spriteOverride(size_t(p.index), sprite);
    if (!overridden && !plates.loadSprite(size_t(idx), sprite)) return false;
    const bool enlarged = dw * 2 > sprite.w * 3 || dh * 2 > sprite.h * 3;
    drawSprite(canvas, sprite, margin + p.x, originY + p.y, dw, dh, flips[size_t(p.index)],
               enlarged ? settings.resample : Resample::Bilinear);
  }
  const int t2 = nowMs();

  if (settings.cream > 0) creamPaper(canvas, settings.cream);
  if (settings.beforeDither) settings.beforeDither(canvas);
  progress("dithering to six inks");
  dither(canvas, out, settings.vivid, settings.sharpen, settings.edges, settings.jitter);
  canvas.px.clear();
  canvas.px.shrink_to_fit();
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
      drawName(out, firstFace(first), font, first, second, margin + p.labelX, originY + p.labelY,
               p.labelW, p.labelH, px, subScale);
    }
  }

  const int left = margin + dateInset, right = width - margin - dateInset;
  // Both lines share a baseline: the date's, or the note's own when alone.
  const int ascent = dated ? dateAscent : noteAscent, descent = dated ? dateDescent : noteDescent;
  const int baseline =
      dateTop ? margin + dateInset + ascent : height - margin - dateInset - descent;
  if (dated) {
    progress("writing the date");
    const int x = settings.dateAlign == DateAlign::Left     ? left
                  : settings.dateAlign == DateAlign::Centre ? (width - dateW) / 2
                                                            : right - dateW;
    font.draw(out, settings.date, x, baseline, namePx, kBlack);
  }
  if (noted) {
    // The end the date is not at; the left when the date is centred or absent.
    const bool noteRight = dated && settings.dateAlign == DateAlign::Left;
    font.draw(out, settings.note, noteRight ? right - noteW : left, baseline, notePx, kBlack);
  }

  if (report) {
    report->placed = int(placed.size());
    report->labelPx = names ? usedPx : 0;
    report->packMs = t1 - t0;
    report->drawMs = t2 - t1;
    report->ditherMs = t3 - t2;
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
