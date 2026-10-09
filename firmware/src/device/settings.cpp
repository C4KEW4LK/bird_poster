#include "settings.h"

#include <algorithm>

#include <Preferences.h>

namespace birdposter {

namespace {

std::string getStr(Preferences &p, const char *key, const std::string &fallback) {
  if (!p.isKey(key)) return fallback;
  return std::string(p.getString(key, fallback.c_str()).c_str());
}

}  // namespace

void loadSettings(Settings &s) {
  Preferences p;
  if (!p.begin("frame", true)) return;
  s.wifiSsid = getStr(p, "ssid", s.wifiSsid);
  s.wifiPass = getStr(p, "pass", s.wifiPass);
  s.apPass = getStr(p, "appass", s.apPass);
  s.hostname = getStr(p, "host", s.hostname);
  // 0 BirdNET-Go, 1 iNaturalist, 2 eBird, 3 ALA, 4 a JSON list; anything
  // else is the default.
  s.source = Source(std::min<int>(4, p.getUChar("source", 1)));
  s.mode = p.getUChar("mode", 0) == 1 ? Mode::Rarest : Mode::MostDetected;
  s.detectorUrl = getStr(p, "detector", s.detectorUrl);
  s.ebirdKey = getStr(p, "ebirdkey", s.ebirdKey);
  s.ebirdLocale = getStr(p, "ebirdloc", s.ebirdLocale);
  s.listUrl = getStr(p, "listurl", s.listUrl);
  s.lat = p.getDouble("lat", s.lat);
  s.lng = p.getDouble("lng", s.lng);
  s.radiusKm = p.getInt("radius", s.radiusKm);
  s.lookback = p.getInt("lookb", s.lookback);
  s.lookbackUnit = Settings::Lookback(p.getUChar("lookbu", uint8_t(s.lookbackUnit)) & 3);
  s.inatVersion = p.getUChar("inatv", uint8_t(s.inatVersion)) == 1 ? 1 : 2;
  s.birds = p.getInt("birds", s.birds);
  s.everyBird = p.getBool("everybird", s.everyBird);
  s.rotation = p.getInt("rotation", s.rotation) & 3;
  // A new key: the old "names" was a bool, and reading it as a style would
  // turn "on" into "Scientific only".
  s.names = NameStyle(p.getUChar("namestyle", uint8_t(s.names)) & 3);
  s.commonCase = NameCase(std::min<int>(2, p.getUChar("namecase", uint8_t(s.commonCase))));
  s.labelSize = LabelSize(p.getUChar("label", uint8_t(s.labelSize)) & 3);
  s.sciPercent = std::clamp<int>(p.getUChar("scipct", uint8_t(s.sciPercent)), 40, 100);
  // Unknown values fall back to the spiral, so a pack style saved by a later
  // firmware cannot leave an older one with no layout at all.
  const uint8_t style = p.getUChar("packstyle", uint8_t(s.packStyle));
  s.packStyle = style <= uint8_t(PackStyle::Hero) ? PackStyle(style) : PackStyle::Classic;
  s.shuffleBirds = p.getBool("shuffle", s.shuffleBirds);
  s.preferNew = p.getBool("newfirst", s.preferNew);
  s.minConfidence = p.getInt("bnconf", s.minConfidence);
  s.webPlates = p.getBool("webplates", s.webPlates);
  s.webPlatesUrl = getStr(p, "weburl", s.webPlatesUrl);
  s.dateOrder = DateOrder(std::min<int>(1, p.getUChar("dateorder", uint8_t(s.dateOrder))));
  s.topText = getStr(p, "toptext", s.topText);
  s.bottomText = getStr(p, "bottext", s.bottomText);
  s.topAlign = TextAlign(std::min<int>(2, p.getUChar("topalign", uint8_t(s.topAlign))));
  s.bottomAlign = TextAlign(std::min<int>(2, p.getUChar("botalign", uint8_t(s.bottomAlign))));
  // "textface" was one face for both lines, before each had its own.
  const bool both = p.getBool("textface", false);
  s.topInNameFont = p.getBool("topface", both || s.topInNameFont);
  s.bottomInNameFont = p.getBool("botface", both || s.bottomInNameFont);
  // The date was a setting of its own before the page had lines of text: a
  // frame that showed it gets the same date as a line on the same edge, and
  // saveSettings drops the old keys so this happens once.
  if (p.getBool("date", false)) {
    static const char *const kStyles[] = {"{{date.short}}", "{{date.numeric}}", "{{date.medium}}",
                                          "{{date.long}}", "{{date.full}}"};
    const int style = std::min<int>(4, p.getUChar("datestyle", 3));
    const bool top = p.getUChar("dateedge", 1) == 0;
    const TextAlign align = TextAlign(std::min<int>(2, p.getUChar("datealign", 2)));
    std::string &line = top ? s.topText : s.bottomText;
    if (line.empty()) {
      line = kStyles[style];
      (top ? s.topAlign : s.bottomAlign) = align;
    }
  }
  // 0 was "the names' size": the size the names are set to, then. The name
  // sizes run Small to Extra large as 0 to 3, the line sizes as 1 to 4.
  const auto lineSize = [&](const char *key, TextSize fallback) {
    const int v = p.getUChar(key, uint8_t(fallback));
    return TextSize(v == 0 ? int(s.labelSize) + 1 : std::clamp(v, 1, 5));
  };
  s.topSize = lineSize("topsize", s.topSize);
  s.bottomSize = lineSize("botsize", s.bottomSize);
  // Counting refreshes was a setting of its own, which wrote the count small
  // along the bottom; now the frame always counts and {{refresh}} places it.
  // A frame that had it on keeps it, as a line, once.
  if (p.getBool("countref", false) && s.bottomText.empty()) {
    s.bottomText = "Refresh {{refresh}}";
    s.bottomAlign = TextAlign::Left;
    s.bottomSize = TextSize::Small;
  }
  s.showConfidence = p.getBool("showconf", s.showConfidence);
  s.newText = p.getBool("newtext", s.newText);
  s.miles = p.getBool("miles", s.miles);
  s.fahrenheit = p.getBool("fahr", s.fahrenheit);
  s.clock12h = p.getBool("clock12", s.clock12h);
  s.newTopText = getStr(p, "newtop", s.newTopText);
  s.newBottomText = getStr(p, "newbot", s.newBottomText);
  s.vivid = std::min<int>(4, p.getUChar("vivid", uint8_t(s.vivid)));
  s.sharpen = std::min<int>(4, p.getUChar("sharpen", uint8_t(s.sharpen)));
  s.edges = std::min<int>(4, p.getUChar("edges", uint8_t(s.edges)));
  s.cream = std::min<int>(4, p.getUChar("cream", uint8_t(s.cream)));
  // The renderer clamps these to a quarter of their axis; the 300 here is
  // only so a corrupt key cannot ask for a page-sized border.
  const auto marginPx = [&p](const char *key, int fallback) {
    return std::clamp<int>(p.getInt(key, fallback), 0, 300);
  };
  s.marginPerSide = p.getBool("marginps", s.marginPerSide);
  s.margin = marginPx("margin", s.margin);
  s.marginTop = marginPx("margint", s.marginTop);
  s.marginRight = marginPx("marginr", s.marginRight);
  s.marginBottom = marginPx("marginb", s.marginBottom);
  s.marginLeft = marginPx("marginl", s.marginLeft);
  s.cycleHours = p.getInt("cycleh", s.cycleHours);  // new key: "cycle" was days
  s.intervalMin = p.getInt("interval", s.intervalMin);
  // New keys in minutes; the old ones held whole hours.
  const auto quiet = [&p](const char *key, const char *oldKey, int fallback) {
    if (p.isKey(key)) return std::clamp<int>(p.getInt(key, fallback), 0, 24 * 60 - 1);
    if (p.isKey(oldKey)) return std::clamp<int>(p.getInt(oldKey, 0), 0, 23) * 60;
    return fallback;
  };
  s.quietFrom = quiet("quietfromm", "quietfrom", s.quietFrom);
  s.quietTo = quiet("quiettom", "quietto", s.quietTo);
  s.tz = getStr(p, "tz", s.tz);
  s.pack = getStr(p, "pack", s.pack);
  p.end();
}

void saveSettings(const Settings &s) {
  Preferences p;
  if (!p.begin("frame", false)) return;
  p.putString("ssid", s.wifiSsid.c_str());
  p.putString("pass", s.wifiPass.c_str());
  p.putString("appass", s.apPass.c_str());
  p.putString("host", s.hostname.c_str());
  p.putUChar("source", uint8_t(s.source));
  p.putUChar("mode", s.mode == Mode::Rarest ? 1 : 0);
  p.putString("detector", s.detectorUrl.c_str());
  p.putString("ebirdkey", s.ebirdKey.c_str());
  p.putString("ebirdloc", s.ebirdLocale.c_str());
  p.putString("listurl", s.listUrl.c_str());
  p.putDouble("lat", s.lat);
  p.putDouble("lng", s.lng);
  p.putInt("radius", s.radiusKm);
  p.putInt("lookb", s.lookback);
  p.putUChar("lookbu", uint8_t(s.lookbackUnit));
  p.putUChar("inatv", uint8_t(s.inatVersion));
  p.putInt("birds", s.birds);
  p.putBool("everybird", s.everyBird);
  p.putInt("rotation", s.rotation & 3);
  p.putUChar("namestyle", uint8_t(s.names));
  p.putUChar("namecase", uint8_t(s.commonCase));
  p.putUChar("label", uint8_t(s.labelSize));
  p.putUChar("scipct", uint8_t(s.sciPercent));
  p.putUChar("packstyle", uint8_t(s.packStyle));
  p.putBool("shuffle", s.shuffleBirds);
  p.putBool("newfirst", s.preferNew);
  p.putInt("bnconf", s.minConfidence);
  p.putBool("webplates", s.webPlates);
  p.putString("weburl", s.webPlatesUrl.c_str());
  p.putUChar("dateorder", uint8_t(s.dateOrder));
  for (const char *gone : {"date", "datestyle", "dateedge", "datealign", "countref"})  // now lines of text
    if (p.isKey(gone)) p.remove(gone);
  p.putString("toptext", s.topText.c_str());
  p.putString("bottext", s.bottomText.c_str());
  p.putUChar("topalign", uint8_t(s.topAlign));
  p.putUChar("botalign", uint8_t(s.bottomAlign));
  p.putBool("topface", s.topInNameFont);
  p.putBool("botface", s.bottomInNameFont);
  if (p.isKey("textface")) p.remove("textface");
  p.putUChar("topsize", uint8_t(s.topSize));
  p.putUChar("botsize", uint8_t(s.bottomSize));
  p.putBool("showconf", s.showConfidence);
  p.putBool("newtext", s.newText);
  p.putBool("miles", s.miles);
  p.putBool("fahr", s.fahrenheit);
  p.putBool("clock12", s.clock12h);
  p.putString("newtop", s.newTopText.c_str());
  p.putString("newbot", s.newBottomText.c_str());
  p.putUChar("vivid", uint8_t(s.vivid));
  p.putUChar("sharpen", uint8_t(s.sharpen));
  p.putUChar("edges", uint8_t(s.edges));
  p.putUChar("cream", uint8_t(s.cream));
  p.putBool("marginps", s.marginPerSide);
  p.putInt("margin", s.margin);
  p.putInt("margint", s.marginTop);
  p.putInt("marginr", s.marginRight);
  p.putInt("marginb", s.marginBottom);
  p.putInt("marginl", s.marginLeft);
  p.putInt("cycleh", s.cycleHours);
  p.putInt("interval", s.intervalMin);
  p.putInt("quietfromm", s.quietFrom);
  p.putInt("quiettom", s.quietTo);
  p.putString("tz", s.tz.c_str());
  p.putString("pack", s.pack.c_str());
  p.end();
}

void loadState(State &s) {
  Preferences p;
  if (!p.begin("state", true)) return;
  s.layout = p.getInt("layout", 0);
  s.wifiFailures = p.getInt("wififail", 0);
  s.lastRender = p.getUInt("lastrender", 0);
  s.lastResult = getStr(p, "result", "");
  s.lastBirds = getStr(p, "birds", "");
  s.fetchOk = p.getBool("fetchok", false);
  s.lastHttp = p.getInt("http", 0);
  s.showingStatus = p.getBool("status", false);
  s.portalOn = p.getBool("portal", false);
  s.glass = getStr(p, "glass", "");
  s.refreshes = p.getUInt("refreshes", 0);
  s.refreshesSince = p.getUInt("refsince", 0);
  s.pageSig = p.getUInt("pagesig", 0);
  p.end();
}

void saveState(const State &s) {
  Preferences p;
  if (!p.begin("state", false)) return;
  p.putInt("layout", s.layout);
  p.putInt("wififail", s.wifiFailures);
  p.putUInt("lastrender", s.lastRender);
  p.putString("result", s.lastResult.c_str());
  p.putString("birds", s.lastBirds.c_str());
  p.putBool("fetchok", s.fetchOk);
  p.putInt("http", s.lastHttp);
  p.putBool("status", s.showingStatus);
  p.putBool("portal", s.portalOn);
  p.putString("glass", s.glass.c_str());
  p.putUInt("refreshes", s.refreshes);
  p.putUInt("refsince", s.refreshesSince);
  p.putUInt("pagesig", s.pageSig);
  p.end();
}

}  // namespace birdposter
