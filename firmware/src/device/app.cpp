#include "app.h"
#include "timing.h"

#include <cctype>

#include <iterator>

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_mac.h>
#if defined(BOARD_E1004)
#include <SD.h>
#endif

#include <algorithm>
#include <map>
#include <new>
#include <random>

namespace birdposter {

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif
#ifdef BIRDPOSTER_DEBUG
const char *kFirmwareVersion = FIRMWARE_VERSION "-debug";
#else
const char *kFirmwareVersion = FIRMWARE_VERSION;
#endif

namespace {

// Reader for the pack, over whichever filesystem it came from - LittleFS in
// flash or FAT on the card; both hand out the same fs::File. The handle stays
// open for the life of the App: every bird on a page seeks into it.
File platesFile;

bool readAt(uint32_t offset, void *dst, size_t len) {
  if (!platesFile) return false;
  if (!platesFile.seek(offset)) return false;
  return platesFile.read(static_cast<uint8_t *>(dst), len) == len;
}

}  // namespace

void App::progress(const std::string &what) {
  phase = what;
  if (!what.empty()) Serial.printf("… %s\n", what.c_str());
  if (onProgress) onProgress();
}

bool App::begin() {
  // Before anything else asks PSRAM for memory: see pageMemory.
  if (!pageMemory) {
    const size_t px = size_t(kPageLong) * kPackShort;
    pageMemory = static_cast<uint16_t *>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM));
    pageMemoryPx = pageMemory ? px : 0;
    if (!pageMemory) Serial.println("page memory: could not reserve; pages will allocate");
  }
  {
    timing::Scope t(timing::BeginSettings);
    loadSettings(settings);
    loadState(state);
  }
  panel.onWait = [this] {
    if (onProgress) onProgress();
  };

  // By label: the default is "spiffs", and ours is called `plates` in the
  // partition table. The subtype is what pio's uploadfs goes by; the label is
  // what the mount goes by, and only the second one is ours to get wrong.
  uint32_t at = millis();
  if (!LittleFS.begin(false, "/littlefs", 10, "plates")) {
    platesError = "no filesystem - flash the plates image";
    return false;
  }
  timing::add(timing::BeginMount, millis() - at);
  at = millis();
  openPack();
  timing::add(timing::BeginPack, millis() - at);

  timing::Scope fonts(timing::BeginFonts);
  File f = LittleFS.open(kFontPath, "r");
  if (f) {
    std::vector<uint8_t> bytes(f.size());
    fontOk = f.read(bytes.data(), bytes.size()) == bytes.size() && font.load(std::move(bytes));
    f.close();
  }
  if (File nf = LittleFS.open(kNameFontPath, "r")) {
    std::vector<uint8_t> bytes(nf.size());
    if (nf.read(bytes.data(), bytes.size()) == bytes.size()) nameFont.load(std::move(bytes));
    nf.close();
    nameFont.setKeepHairlines(true);  // Gould's hairlines; see Font::setKeepHairlines
  }
  loadShown();
  loadNewToday();
  return platesOk && fontOk;
}

bool App::mountCard() {
#if defined(BOARD_E1004)
  if (cardOk) return true;
  pinMode(pins::kSdDetect, INPUT_PULLUP);
  if (digitalRead(pins::kSdDetect) == HIGH) {
    cardError = "no card in the slot";
    return false;
  }
  pinMode(pins::kSdEnable, OUTPUT);
  digitalWrite(pins::kSdEnable, HIGH);
  delay(10);
  // 20 MHz: the bus is shared with the panel's traces and the card is read a
  // few hundred KB at a time, so nothing here wants the last MHz.
  if (!SD.begin(pins::kSdCs, sharedSpi(), 20000000, "/sd", 5, false)) {
    cardError = "card in the slot but it would not mount - FAT32, under 32 GB";
    digitalWrite(pins::kSdEnable, LOW);
    return false;
  }
  cardOk = true;
  cardError.clear();
  Serial.printf("sd: %llu MB card\n", (unsigned long long)(SD.cardSize() >> 20));
  return true;
#else
  cardError = "this board has no SD slot";
  return false;
#endif
}

void App::unmountCard() {
#if defined(BOARD_E1004)
  if (!cardOk) return;
  if (platesFile && packOnCard) platesFile.close();
  SD.end();
  digitalWrite(pins::kSdEnable, LOW);
  cardOk = false;
#endif
}

namespace {

// The region keys of the packs in a filesystem's root: "/plates-au.bin" is
// "au". `single` reports a lone /plates.bin, the 16 MB board's one pack.
void listPacks(fs::FS &fs, std::vector<std::string> &keys, bool *single) {
  File dir = fs.open("/");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    std::string name = f.name();
    if (name.rfind("/", 0) == 0) name.erase(0, 1);
    if (name == "plates.bin") {
      if (single) *single = true;
    } else if (name.rfind("plates-", 0) == 0 && name.size() > 11 &&
               name.compare(name.size() - 4, 4, ".bin") == 0) {
      keys.push_back(name.substr(7, name.size() - 11));
    }
  }
}

}  // namespace

bool App::openPack() {
  // What is on offer: /plates.bin, one region's, in flash - or, on an E1004
  // flashed with the old every-region image, one /plates-<region>.bin per
  // region - and on the E1004 /plates-<region>.bin at the root of the SD
  // card, where the pack is the full-size one the flasher page hands out for
  // copying. The setting picks a region; the card wins for a region it holds;
  // an unset or missing choice falls back to the first pack found, so a fresh
  // board draws something before anyone opens the settings.
  packs.clear();
  cardPacks.clear();
  bool single = false;
  std::vector<std::string> flashPacks;
  listPacks(LittleFS, flashPacks, &single);
#if defined(BOARD_E1004)
  if (mountCard()) listPacks(SD, cardPacks, nullptr);
#endif
  std::sort(flashPacks.begin(), flashPacks.end());
  std::sort(cardPacks.begin(), cardPacks.end());
  packs = cardPacks;
  for (const std::string &key : flashPacks)
    if (std::find(packs.begin(), packs.end(), key) == packs.end()) packs.push_back(key);
  std::sort(packs.begin(), packs.end());

  std::string key;
  if (!settings.pack.empty() && std::find(packs.begin(), packs.end(), settings.pack) != packs.end())
    key = settings.pack;
  else if (!single && !packs.empty())
    key = packs.front();
  if (platesFile) platesFile.close();
  platesOk = false;
  packKey = key;
  // A lone /plates.bin does not say which region it is; the flasher writes
  // /region.txt beside it so the web supplement knows where to ask.
  packRegion = key;
  if (packRegion.empty()) {
    if (File r = LittleFS.open(kRegionPath, "r")) {
      packRegion = r.readStringUntil('\n').c_str();
      r.close();
      while (!packRegion.empty() && std::isspace(static_cast<unsigned char>(packRegion.back())))
        packRegion.pop_back();
    }
  }
  packOnCard = !key.empty() && std::find(cardPacks.begin(), cardPacks.end(), key) != cardPacks.end();
  packPath = key.empty() ? (single ? kPlatesPath : "") : kPackPrefix + key + ".bin";
  if (packPath.empty()) {
    platesError = cardOk ? "no plate pack on the card or in flash - flash a plates image"
                         : "no plate pack on the filesystem - flash a plates image";
    return false;
  }
#if defined(BOARD_E1004)
  platesFile = packOnCard ? SD.open(packPath.c_str(), "r") : LittleFS.open(packPath.c_str(), "r");
#else
  platesFile = LittleFS.open(packPath.c_str(), "r");
#endif
  if (!platesFile) {
    platesError = "cannot open " + packPath + (packOnCard ? " on the card" : "");
    return false;
  }
  platesOk = plates.open(readAt, &platesError);
  if (platesOk)
    Serial.printf("plates: %s from %s, %u species at %d px\n", packPath.c_str(),
                  packOnCard ? "the SD card" : "flash", (unsigned)plates.count(), plates.source());
  return platesOk;
}

void App::scanNetworks() {
  // Blocking, two or three seconds. Needs the station interface up; the
  // access point is started as AP+STA for exactly this.
  progress("scanning for networks");
  const int n = WiFi.scanNetworks(false, false);
  networks.clear();
  for (int i = 0; i < n; ++i) {
    const std::string ssid(WiFi.SSID(i).c_str());
    if (ssid.empty()) continue;
    const int rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    bool seen = false;
    for (Network &k : networks)
      if (k.ssid == ssid) {
        seen = true;
        if (rssi > k.rssi) k.rssi = rssi;
      }
    if (!seen) networks.push_back(Network{ssid, rssi, secure});
  }
  std::sort(networks.begin(), networks.end(),
            [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  if (networks.size() > 20) networks.resize(20);
  WiFi.scanDelete();
  scanned = true;
  progress("");
}

void App::loadShown() {
  shown.clear();
  File f = LittleFS.open(kShownPath, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    const int sp = line.indexOf(' ');
    if (sp <= 0) continue;
    shown[std::string(line.c_str() + sp + 1)] = std::time_t(line.substring(0, sp).toInt());
  }
  f.close();
}

void App::recordShown(const std::vector<std::string> &names) {
  const std::time_t now = std::time(nullptr);
  if (now < 100000) return;  // no clock: a wrong date would hide birds for a year
  for (const std::string &n : names) shown[n] = now;
  // Keep only what the window can still care about, with headroom for a
  // window that grows later.
  const std::time_t keep = now - std::time_t(std::max(settings.cycleHours, 30 * 24)) * 2 * 60 * 60;
  for (auto it = shown.begin(); it != shown.end();)
    it = it->second < keep ? shown.erase(it) : std::next(it);
  File f = LittleFS.open(kShownPath, "w");
  if (!f) return;
  for (const auto &[name, at] : shown) f.printf("%ld %s\n", long(at), name.c_str());
  f.close();
}

void App::loadNewToday() {
  newToday.clear();
  File f = LittleFS.open(kNewTodayPath, "r");
  if (!f) return;
  while (f.available()) {
    const String line = f.readStringUntil('\n');
    const int a = line.indexOf('\t'), b = line.indexOf('\t', a + 1);
    if (a <= 0 || b <= a) continue;
    Sighting s;
    s.newOn = line.substring(0, a).c_str();
    s.scientific = line.substring(a + 1, b).c_str();
    s.common = line.substring(b + 1).c_str();
    newToday.push_back(s);
  }
  f.close();
}

void App::noteNewToday(const std::vector<Sighting> &seen) {
  const std::time_t now = std::time(nullptr);
  if (now < 100000) return;  // no clock, no "today"
  const std::string today = localStamp(now).substr(0, 10);
  bool changed = false;
  for (auto it = newToday.begin(); it != newToday.end();) {
    if (it->newOn == today) {
      ++it;
    } else {
      it = newToday.erase(it);
      changed = true;
    }
  }
  for (const Sighting &s : seen) {
    if (s.newOn != today) continue;
    if (std::any_of(newToday.begin(), newToday.end(),
                    [&s](const Sighting &k) { return k.scientific == s.scientific; }))
      continue;
    newToday.push_back(s);
    changed = true;
  }
  if (!changed) return;
  File f = LittleFS.open(kNewTodayPath, "w");
  if (!f) return;
  for (const Sighting &s : newToday)
    f.printf("%s\t%s\t%s\n", s.newOn.c_str(), s.scientific.c_str(), s.common.c_str());
  f.close();
}

bool App::fetch(const std::string &url, std::string &body, int &http, std::string &error,
                const std::vector<std::pair<std::string, std::string>> &headers,
                uint32_t timeoutMs) {
  const size_t at = url.find("://");
  const size_t end = at == std::string::npos ? std::string::npos : url.find('/', at + 3);
  const std::string host = at == std::string::npos ? url : url.substr(at + 3, end - at - 3);
  progress("asking " + host);
  const bool secure = url.rfind("https://", 0) == 0;
  // A .local name is mDNS, which the ESP32's resolver does not consult on its
  // own: ask the network for it first, and say so when nobody answers, which
  // is the usual reason a BirdNET-Go address fails.
  const size_t colon = host.find(':');
  const std::string hostname = host.substr(0, colon);  // npos takes the lot
  const bool local = hostname.size() > 6 && hostname.compare(hostname.size() - 6, 6, ".local") == 0;
  std::string target = url;
  if (local) {
    const std::string name = hostname.substr(0, hostname.size() - 6);
    const IPAddress ip = MDNS.queryHost(name.c_str(), 3000);
    if (ip == IPAddress(0, 0, 0, 0)) {
      http = 0;
      error = "no reply: nothing on this network calls itself " + name + ".local - check the "
              "address, or use the IP";
      return false;
    }
    target = url.substr(0, at + 3) + ip.toString().c_str() +
             (colon == std::string::npos ? "" : host.substr(colon)) + url.substr(end == std::string::npos ? url.size() : end);
  }
  HTTPClient client;
  client.setUserAgent(kUserAgent);
  client.setTimeout(timeoutMs);
  client.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  NetworkClientSecure tls;
  bool ok;
  if (secure) {
    tls.setInsecure();
    ok = client.begin(tls, target.c_str());
  } else {
    ok = client.begin(target.c_str());
  }
  if (!ok) {
    http = 0;
    error = "bad URL: " + url;
    return false;
  }
  for (const auto &[name, value] : headers) client.addHeader(name.c_str(), value.c_str());
  http = client.GET();
  if (http <= 0) {
    // HTTPClient's own words, then what they usually mean at this address.
    error = std::string("no reply from ") + host + ": " + HTTPClient::errorToString(http).c_str();
    switch (http) {
      case HTTPC_ERROR_CONNECTION_REFUSED:
        error += secure ? " - the name did not resolve or the host is unreachable; is the frame's "
                          "network connected to the internet?"
                        : " - nothing is listening there; check the host and the port, and that "
                          "the frame and the server are on the same network";
        break;
      case HTTPC_ERROR_READ_TIMEOUT:
        error += " - it accepted the connection but sent nothing within " +
                 std::to_string(timeoutMs / 1000) + " s";
        break;
      case HTTPC_ERROR_CONNECTION_LOST:
        error += " - the connection dropped mid-reply; a weak WiFi signal does this";
        break;
      default:
        break;
    }
    client.end();
    return false;
  }
  // Let HTTPClient copy the body out: it is the one that knows whether the
  // reply is chunked. Reading the socket directly, as this once did, left
  // the chunk lengths in the body - "4200\r\n{" - which ArduinoJson reads
  // as the number 4200 and then reports as a reply of the wrong shape.
  // The sink is a Stream that appends to the string, so the body is built
  // once, in place, rather than through an Arduino String.
  struct Sink : public Stream {
    std::string &out;
    App &app;
    size_t reported = 0;
    Sink(std::string &o, App &a) : out(o), app(a) {}
    size_t write(uint8_t c) override {
      out.push_back(char(c));
      return 1;
    }
    size_t write(const uint8_t *buf, size_t n) override {
      out.append(reinterpret_cast<const char *>(buf), n);
      if (out.size() - reported >= 32768) {
        reported = out.size();
        app.progress("reading the reply, " + std::to_string(out.size() / 1024) + " KB");
      }
      return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
  };
  const int declared = client.getSize();
  body.clear();
  body.reserve(declared > 0 ? size_t(declared) : 32768);
  Sink sink(body, *this);
  const int wrote = client.writeToStream(&sink);
  client.end();
  if (wrote < 0) {
    error = std::string("reply from ") + host + " cut short: " + HTTPClient::errorToString(wrote).c_str() +
            " - " + std::to_string(body.size() / 1024) + " KB arrived";
    return false;
  }
  return true;
}

std::time_t App::windowStart(int lookback, Settings::Lookback unit) const {
  const std::time_t now = std::time(nullptr);
  if (now < 100000) return 0;  // no clock: no window is better than a wrong one
  switch (unit) {
    case Settings::Lookback::Minutes: return now - std::time_t(std::max(1, lookback)) * 60;
    case Settings::Lookback::Hours: return now - std::time_t(std::max(1, lookback)) * 3600;
    case Settings::Lookback::Days: return now - std::time_t(std::max(1, lookback)) * 86400;
    case Settings::Lookback::SinceLast:
      // Nothing drawn yet: a day, so the first page is not empty.
      return state.lastRender > 100000 ? std::time_t(state.lastRender) : now - 86400;
  }
  return 0;
}

std::string App::localStamp(std::time_t t) const {
  if (t <= 0) return "";
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

SourceConfig App::sourceConfig() const {
  SourceConfig cfg;
  cfg.source = settings.source;
  cfg.detectorUrl = settings.detectorUrl;
  cfg.lat = settings.lat;
  cfg.lng = settings.lng;
  cfg.radiusKm = settings.radiusKm;
  cfg.since = windowStart(settings.lookback, settings.lookbackUnit);
  cfg.inatVersion = settings.inatVersion;
  cfg.ebirdKey = settings.ebirdKey;
  cfg.minConfidence = settings.minConfidence;
  cfg.ebirdLocale = settings.ebirdLocale;
  cfg.listUrl = settings.listUrl;
  // Every bird in the window wants the whole window: the newest 200 calls can
  // be a quarter of an hour of dawn chorus, and a species heard before them
  // would be missed.
  if (everyBird()) cfg.limit = 1000;
  return cfg;
}

bool App::querySource(const SourceConfig &cfg, Mode mode, std::vector<Sighting> &seen, int &http,
                      std::string &error) {
  seen.clear();
  http = 0;
  if (!supports(cfg.source, mode)) {
    error = "this source cannot rank by rarity";
    return false;
  }
  const std::string url = requestUrl(cfg, std::time(nullptr), mode);
  std::string body;
  const uint32_t httpAt = millis();
  const bool got = fetch(url, body, http, error, requestHeaders(cfg));
  timing::add(timing::FetchHttp, millis() - httpAt);
  timing::set(timing::FetchBytes, int32_t(body.size()));
  if (!got) return false;
  if (http != 200) {
    error = explainStatus(cfg.source, http, body);
    return false;
  }
  progress("reading the species list, " + std::to_string(body.size() / 1024) + " KB");
  timing::Scope parse(timing::FetchParse);
  if (!parseResponse(cfg.source, body, seen, &error, localStamp(cfg.since), cfg.minConfidence))
    return false;
  return true;
}

bool App::fetchBirds(std::vector<int> &plateIndices) {
  fetchError.clear();
  std::vector<Sighting> seen;
  int http = 0;
  // The whole of it, choosing included; querySource times its own parts.
  timing::Scope fetching(timing::Fetch);
  const bool got = querySource(sourceConfig(), settings.mode, seen, http, fetchError);
  state.lastHttp = http;
  if (!got) return false;
  progress("choosing from " + std::to_string(seen.size()) + " species");
  const auto drawable = [this](const std::string &name) { return plates.find(name) >= 0; };
  // Every bird: as many as were heard, up to `birds`, the most-heard first
  // if there are more. Cycling has nothing to rotate through then.
  const bool every = everyBird();
  const size_t want = size_t(std::max(1, std::min(settings.birds, 40)));
  std::vector<Sighting> page = choose(seen, drawable, settings.mode, want);
  if (settings.cycleHours > 0 && !every) {
    // Rank everything drawable, then rotate: random among what the window
    // has not shown, topped up with the longest-unseen.
    const std::vector<Sighting> ranked = choose(seen, drawable, settings.mode, seen.size());
    const auto lastShown = [this](const std::string &name) {
      const auto it = shown.find(name);
      return it == shown.end() ? std::time_t(0) : it->second;
    };
    page = cycle(ranked, lastShown, std::time(nullptr), settings.cycleHours, want,
                 uint32_t(std::time(nullptr)) ^ uint32_t(state.layout));
  }
  // New birds first: BirdNET-Go's own word for it - a species it first heard
  // today (Sighting::newOn) - and kept first for the rest of that day, as
  // BirdNET-Go keeps its "new" badge, even once the window has moved past
  // the detection. No other source says.
  if (settings.source == Source::BirdNet) {
    noteNewToday(seen);
    // Only onto a page there is anyway: nothing heard keeps the last page up,
    // and a remembered bird alone is not a reason to redraw it.
    if (settings.preferNew && !newToday.empty() && !seen.empty()) {
      std::vector<Sighting> pool = choose(seen, drawable, settings.mode, seen.size());
      for (const Sighting &k : newToday) {
        const bool there = std::any_of(pool.begin(), pool.end(),
                                       [&k](const Sighting &s) { return s.scientific == k.scientific; });
        if (!there && drawable(k.scientific)) pool.push_back(k);  // not heard again since
      }
      const auto isNew = [this](const std::string &name) {
        return std::any_of(newToday.begin(), newToday.end(),
                           [&name](const Sighting &k) { return k.scientific == name; });
      };
      page = preferNew(page, pool, isNew, want);
    }
  }
  if (settings.shuffleBirds) {
    // Seeded by the layout, which every page moves on: a new order each page,
    // and the same one again for the same layout.
    std::mt19937 rng(uint32_t(state.layout) * 2654435761u ^ 0x5bd1e995u);
    std::shuffle(page.begin(), page.end(), rng);
  }
  plateIndices.clear();
  pageBirds.clear();
  pageCommon.clear();
  for (const Sighting &s : page) {
    plateIndices.push_back(plates.find(s.scientific));
    pageBirds.push_back(s.scientific);
    pageCommon.push_back(s.common);
  }
  if (plateIndices.empty()) {
    fetchError = every && seen.empty()
                     ? "nothing detected since " + localTime(windowStart(settings.lookback, settings.lookbackUnit))
                     : "no sighting matched a plate (" + std::to_string(seen.size()) + " species seen)";
    return false;
  }
  return true;
}

App::Probe App::probe(const SourceConfig &cfg, Mode mode) {
  Probe r;
  r.url = requestUrl(cfg, std::time(nullptr), mode);
  std::vector<Sighting> seen;
  const bool got = querySource(cfg, mode, seen, r.http, r.error);
  progress("");
  if (!got) return r;
  r.ok = true;
  r.seen = seen.size();
  const auto drawable = [this](const std::string &name) { return plates.find(name) >= 0; };
  for (const Sighting &s : choose(seen, drawable, mode, seen.size())) {
    ++r.drawable;
    if (r.sample.size() < 8) r.sample.push_back(s.common.empty() ? s.scientific : s.common);
  }
  return r;
}

bool App::showBirds(const std::vector<int> &plateIndices) {
  BirdPageSettings ps;
  ps.portrait = settings.portrait();
  ps.names = settings.names;
  ps.labelSize = settings.labelSize;
  ps.subNamePercent = settings.sciPercent;
  ps.packStyle = settings.packStyle;
  ps.variant = state.layout;
  ps.vivid = settings.vivid;
  ps.sharpen = settings.sharpen;
  ps.edges = settings.edges;
  ps.cream = settings.cream;
  ps.marginTop = settings.marginTopPx();
  ps.marginRight = settings.marginRightPx();
  ps.marginBottom = settings.marginBottomPx();
  ps.marginLeft = settings.marginLeftPx();
  ps.commonNames = pageCommon;
  ps.commonCase = settings.commonCase;
  ps.commonFont = &nameFont;
  ps.note = refreshNote();
  // The full-size supplement: for a bird drawn well above its flash plate's
  // size (the renderer only asks then), the same plate at full size from the
  // web - and the flash plate on any failure. After the first failure the
  // rest of the page stays in flash, so a site that is down costs one
  // timeout, not one per bird.
  int webFetched = 0, webFailed = 0, webSkipped = 0, webOom = 0;
  uint32_t webMs = 0;
  std::string webError;
  const bool needsRegion = settings.webPlatesUrl.find("{region}") != std::string::npos;
  // The plates' replies, fetched once the layout says which birds want one
  // (afterLayout, below) and decoded as each is drawn - so the radio can go
  // off for the render rather than staying up for a download mid-draw.
  std::map<size_t, std::string> webBodies;
  if (settings.webPlates && !settings.webPlatesUrl.empty() && (!needsRegion || !packRegion.empty()) &&
      WiFi.status() == WL_CONNECTED) {
    ps.spriteOverride = [&, this](size_t index, SpriteImage &out) {
      const auto it = webBodies.find(index);
      if (it == webBodies.end()) return false;  // not fetched: skipped, failed, or after a failure
      const std::string body = std::move(it->second);
      webBodies.erase(it);
      // A full-size plate decodes to about 2 MB, its luma plane alone 1.4 MB
      // in one piece, beside the canvas. PSRAM fragments - the WiFi and lwIP
      // buffers live there too - so ask before decoding, and draw this bird
      // from flash when there is not the room.
      if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < (1600u << 10) ||
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (3072u << 10)) {
        ++webSkipped;
        return false;
      }
      std::string error;
      bool got = false;
      try {
        got = Plates::decodeSingle(body, out, &error);
      } catch (const std::bad_alloc &) {
        // This bird from flash, and the rest of the page as it was: not the
        // whole render thrown away and done again.
        out = SpriteImage();
        error = "out of memory decoding it";
        ++webOom;
      }
      if (!got) {
        ++webFailed;
        webError = error;
        Serial.printf("web plate: %s; using flash\n", error.c_str());
        return false;
      }
      ++webFetched;
      Serial.printf("web plate: %dx%d\n", out.w, out.h);
      return true;
    };
  }
  ps.afterLayout = [&, this](const std::vector<size_t> &larger) {
    if (ps.spriteOverride) {
      // The replies are ~100 KB each and held until their bird is drawn, so
      // stop well short of crowding the decode.
      size_t held = 0;
      for (const size_t index : larger) {
        if (webFailed || index >= plateIndices.size() || held > (1536u << 10)) break;
        const std::string url = webPlateUrl(plates.entry(size_t(plateIndices[index])).name);
        std::string body, error;
        int http = 0;
        progress("fetching a full-size plate");
        const uint32_t webAt = millis();
        bool got = false;
        try {
          got = fetch(url, body, http, error, {}, 8000) && http == 200;
        } catch (const std::bad_alloc &) {
          error = "out of memory fetching it";
          ++webOom;
        }
        webMs += millis() - webAt;
        if (!got) {
          // After the first failure the rest of the page stays in flash, so a
          // site that is down costs one timeout, not one per bird.
          ++webFailed;
          webError = http > 0 && http != 200 ? "HTTP " + std::to_string(http) : error;
          Serial.printf("web plate: %s - %s; using flash\n", url.c_str(), webError.c_str());
          break;
        }
        held += body.size();
        webBodies[index] = std::move(body);
      }
    }
    // Nothing else in the render wants the network: radio off, when this
    // wake was going to turn it off for the refresh anyway.
    if (beforeRender) {
      const std::function<void()> hook = std::move(beforeRender);
      beforeRender = nullptr;
      hook();
    }
  };
  // Only once the clock has been set: a page dated 1970 is worse than none.
  const std::time_t now = std::time(nullptr);
  if (settings.showDate && now > 100000) {
    std::tm tm{};
    localtime_r(&now, &tm);
    ps.date = formatDate(tm, settings.dateStyle, settings.dateOrder);
    ps.dateEdge = settings.dateEdge;
    ps.dateAlign = settings.dateAlign;
  }
  ps.progress = [this](const char *what) { progress(what); };
  // The page on the glass is not wanted to draw the next one, and it is
  // 1.9 MB: kept through the render, a second page in one wake has the old
  // frame, the canvas and a full-size web plate's decode all up at once, which
  // is more than the PSRAM holds. Let it go now; the preview says "nothing
  // composed yet" until the new page is done.
  lastKind.clear();
  last = Frame();
  ps.pageMemory = pageMemory;
  ps.pageMemoryPx = pageMemoryPx;
  // Pack at half resolution: a quarter of the mask work and memory, and on
  // the desktop no bird smaller nor any overlap worse (performance.md).
  ps.packScale = 2;
  // What the planes kept between the silhouette pass and the draw may have:
  // the PSRAM free now, less what the render will want beside them at its
  // peak - the canvas, the frame if this wake has not drawn one yet, the
  // dither's colour table - and room for the sprite being drawn. A full-size
  // plate from the web is the big one: at 1200 px its planes, its reply and
  // its decode come to about 3.5 MB, against 1.5 MB for the flash's own.
  {
    const auto [w, h] = pageSize(ps.portrait);
    // Both come out of pageMemory when it was reserved, not out of what is free.
    const size_t canvas = pageMemory ? 0 : size_t(w) * h * 2;
    const size_t frame = pageMemory ? 0 : size_t(w) * h;
    const size_t sprite = ps.spriteOverride ? (3584u << 10) : (1536u << 10);
    const size_t need = canvas + frame + 65536 * 3 + sprite;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ps.keepLumaBytes = free > need ? free - need : 0;
  }
  BirdPageReport report;
  const uint32_t t0 = millis();
  // Out of PSRAM is a page not drawn, not a reboot: everything the render
  // had allocated unwinds with the exception, and the record says why.
  bool rendered = false;
  renderOutOfMemory = false;
  // The largest piece of PSRAM before the render: the canvas wants 3.8 MB of
  // it in one piece, and a figure well under the free total is fragmentation.
  const bool webOn = bool(ps.spriteOverride);  // the retry below may turn it off
  timing::PageRecord record;
  record.psramLargestKb = int32_t(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
  {
    static const char *const kStyle[] = {"classic", "grid", "scatter", "hero"};
    snprintf(record.layout, sizeof record.layout, "%s %u%s",
             kStyle[std::min<size_t>(3, size_t(settings.packStyle))], unsigned(plateIndices.size()),
             settings.names == NameStyle::None ? "" : " names");
  }
  timing::set(timing::PsramLargestKb, record.psramLargestKb);
  const auto attempt = [&]() {
    try {
      rendered = renderBirdPage(plates, plateIndices, ps, font, last, &report);
      return true;
    } catch (const std::bad_alloc &) {
      last = Frame();
      Serial.printf("render: out of memory (%u KB PSRAM free, largest block %u KB, after unwinding)\n",
                    unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                    unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
      return false;
    }
  };
  if (!attempt()) {
    // The first try's time, less its web fetches, is thrown away: filed as
    // render's "redo" rather than left in its "rest".
    record.redoMs = millis() - t0 - webMs;
    timing::add(timing::RenderRedo, record.redoMs);
    // Out of memory is, so far, always a full-size web plate beside the
    // canvas. The flash plates need a fraction of that: try once more
    // without the web, so the page is drawn rather than skipped.
    if (ps.spriteOverride) {
      record.redone = true;
      Serial.println("render: again, flash plates only");
      ps.spriteOverride = nullptr;
      webFailed = 1;
      webError = "out of memory with a full-size plate";
      report = BirdPageReport();
      renderOutOfMemory = !attempt();
    } else {
      renderOutOfMemory = true;
    }
  }
  timing::add(timing::WebPlate, webMs);
  timing::add(timing::Render, millis() - t0 - webMs);
  record.renderMs = millis() - t0 - webMs;
  record.webFetched = webFetched;
  record.webOom = webOom;
  record.birds = report.placed;
  record.drawn = rendered;
  record.fellBack = report.fellBack;
  timing::page(record);
  if (!rendered) {
    lastKind.clear();
    return false;
  }
  timing::add(timing::RenderMasks, uint32_t(report.masksMs));
  timing::add(timing::RenderLayout, uint32_t(report.layoutMs));
  timing::add(timing::RenderGrow, uint32_t(report.growMs));
  timing::add(timing::RenderDecode, uint32_t(report.decodeMs));
  // spriteOverride's time: the web plates' fetching is done before the draw
  // (webMs, outside the render), so what is left in the draw is their decode.
  timing::add(timing::RenderWebDecode, uint32_t(report.overrideMs));
  timing::add(timing::RenderResample, uint32_t(report.resampleMs));
  timing::add(timing::RenderDither, uint32_t(report.ditherMs));
  timing::add(timing::RenderText, uint32_t(report.textMs));
  timing::set(timing::Birds, report.placed);
  timing::set(timing::Attempts, report.attempts);
  timing::set(timing::SpriteBuilds, report.pack.sprites);
  timing::set(timing::SpriteBuildMs, int32_t(report.pack.spriteUs / 1000));
  timing::set(timing::PlaceSearches, report.pack.searches);
  timing::set(timing::PlaceSearchMs, int32_t(report.pack.searchUs / 1000));
  timing::set(timing::FitChecks, report.pack.fits);
  timing::set(timing::FitCheckMs, int32_t(report.pack.fitUs / 1000));
  timing::set(timing::LumaKept, report.lumaKept);
  timing::set(timing::SpriteScaleMs, int32_t(report.pack.scaleUs / 1000));
  timing::set(timing::SpriteErodeMs, int32_t(report.pack.erodeUs / 1000));
  timing::set(timing::SpriteLabelMs, int32_t(report.pack.labelUs / 1000));
  timing::set(timing::DitherTableMs, int32_t(report.dither.tableUs / 1000));
  timing::set(timing::DitherLoadMs, int32_t(report.dither.loadUs / 1000));
  timing::set(timing::DitherPrepareMs, int32_t(report.dither.prepareUs / 1000));
  timing::set(timing::DitherDiffuseMs, int32_t(report.dither.diffuseUs / 1000));
  timing::set(timing::DitherWaitMs, int32_t(report.dither.waitUs / 1000));
  timing::set(timing::FastKb, int32_t(report.fast.fastBytes / 1024));
  timing::set(timing::FastFallbackKb, int32_t(report.fast.fallbackBytes / 1024));
  if (webOn) timing::set(timing::WebPlates, webFetched);
  if (webOn)
    lastWebPlates = webFailed ? "flash plates; the web failed: " + webError
                  : webSkipped && !webFetched ? "flash plates; not enough free memory for a full-size one"
                  : webFetched ? std::to_string(webFetched) + " full-size from the web"
                               : "not needed - no bird drawn much larger than its flash plate";
  else
    lastWebPlates = !settings.webPlates           ? ""
                    : settings.webPlatesUrl.empty() ? "off - no address set"
                                                    : "off - no WiFi, or {region} in the URL and no region known";
  Serial.printf("page: %d birds, pack %d ms, draw %d ms, dither %d ms, labels %d px (%lu ms)\n",
                report.placed, report.packMs, report.drawMs, report.ditherMs, report.labelPx,
                (unsigned long)(millis() - t0));
  lastKind = "birds";
  if (!presentPages) return true;
  return present();
}

#ifdef BIRDPOSTER_DEBUG
void App::stressTest(int pages) {
  const Settings saved = settings;
  // The most birds the frame draws, so any smaller page is a prefix of it.
  settings.birds = 40;
  std::vector<int> all;
  const bool got = fetchBirds(all);
  settings = saved;
  if (!got) {
    Serial.printf("stress: no species list - %s\n", fetchError.c_str());
    return;
  }
  const std::vector<std::string> allBirds = pageBirds, allCommon = pageCommon;
  // Every combination of these, the bird count moving fastest so heavy and
  // light pages alternate: few birds draw large and pull full-size web
  // plates; forty fill the packer and the kept planes; scatter and grid run
  // grow twenty rounds; hero draws one bird at the page's size.
  static const int kBirds[] = {2, 3, 6, 12, 20, 40};
  static const PackStyle kStyles[] = {PackStyle::Classic, PackStyle::Grid, PackStyle::Scatter,
                                      PackStyle::Hero};
  constexpr int nBirds = sizeof kBirds / sizeof kBirds[0];
  constexpr int nStyles = sizeof kStyles / sizeof kStyles[0];
  presentPages = false;
  for (int i = 0; i < pages; ++i) {
    settings.birds = kBirds[i % nBirds];
    settings.packStyle = kStyles[(i / nBirds) % nStyles];
    settings.names = (i / (nBirds * nStyles)) % 2 ? NameStyle::None : NameStyle::Both;
    ++state.layout;
    const size_t n = std::min(all.size(), size_t(settings.birds));
    pageBirds.assign(allBirds.begin(), allBirds.begin() + long(n));
    pageCommon.assign(allCommon.begin(), allCommon.begin() + long(n));
    progress("stress test, page " + std::to_string(i + 1) + " of " + std::to_string(pages));
    showBirds(std::vector<int>(all.begin(), all.begin() + long(n)));
    Serial.printf("stress: page %d of %d done, %u KB PSRAM free, largest %u KB, heap %u KB\n", i + 1,
                  pages, unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  }
  presentPages = true;
  settings = saved;
  progress("");
}
#endif

void App::pageIntoReserve() {
  last = Frame();
  if (pageMemory) last.px.adopt(reinterpret_cast<uint8_t *>(pageMemory), pageMemoryPx * 2);
}

bool App::showStatus() {
  pageIntoReserve();
  renderStatusPage(settings.portrait(), font, "Bird poster", statusLines(), last);
  lastKind = "status";
  return present();
}

bool App::showSetup(const std::string &ssid, const std::string &pass, const std::string &url,
                    const std::string &note) {
  pageIntoReserve();
  renderSetupPage(settings.portrait(), font, ssid, pass, url, last, note);
  lastKind = "setup";
  return present();
}

bool App::showPattern() {
  const auto [w, h] = pageSize(settings.portrait());
  pageIntoReserve();
  last.reset(w, h, kWhite);
  // Six bars, then a row of the dithered paper tone and a line of text, so
  // one look answers colour order, orientation and whether text is legible.
  for (int i = 0; i < 6; ++i) last.fillRect(i * w / 6, 0, w / 6 + 1, h * 2 / 3, Ink(i));
  Canvas paper;
  paper.reset(w, h / 3, rgb565(kPaperRgb[0], kPaperRgb[1], kPaperRgb[2]));
  Frame strip;
  dither(paper, strip);
  for (int y = 0; y < strip.h; ++y)
    std::copy(strip.row(y), strip.row(y) + w, last.row(h * 2 / 3 + y));
  if (fontOk) font.draw(last, "Bird poster - test pattern - top left is black", w / 20, h - h / 12,
                        h / 24, kBlack);
  lastKind = "pattern";
  return present();
}

bool App::present() {
  timing::Scope whole(timing::Panel);
  const uint32_t initAt = millis();
  const bool ready = panel.ok() || panel.begin();
  timing::add(timing::PanelInit, millis() - initAt);
  if (!ready) {
    Serial.println("panel: BUSY never released - is the FPC seated?");
    return false;
  }
  if (beforeRefresh) {
    const std::function<void()> hook = std::move(beforeRefresh);
    beforeRefresh = nullptr;
    hook();
  }
  // With the radio off there is nothing to serve while the glass works, so
  // the chip sleeps through the wait rather than spinning at full clock.
  panel.sleepWhileBusy = WiFi.getMode() == WIFI_OFF;
  progress("sending the page to the glass");
  const uint32_t t0 = millis();
  if (!panel.push(last, settings.rotation)) {
    Serial.println("panel: frame does not match the rotation");
    progress("");
    return false;
  }
  const uint32_t t1 = millis();
  timing::add(timing::PanelPush, t1 - t0);
  progress("refreshing the glass, about 30 s");
  const bool ok = panel.refresh();
  timing::add(timing::PanelPowerOn, panel.lastRefresh.powerOn);
  timing::add(timing::PanelUpdate, panel.lastRefresh.update);
  timing::add(timing::PanelPowerOff, panel.lastRefresh.powerOff);
  lastPresented = std::time(nullptr);
  progress("");
  Serial.printf("panel: push %lu ms, refresh %lu ms%s\n", (unsigned long)(t1 - t0),
                (unsigned long)(millis() - t1), ok ? "" : " (timed out)");
  {
    timing::Scope t(timing::PanelSleep);
    panel.sleep();
  }
  // Every refresh the panel was asked for drew on the battery, the timed-out
  // ones too, so they all count.
  if (settings.countRefreshes) {
    ++state.refreshes;
    if (!state.refreshesSince && lastPresented > 100000) state.refreshesSince = uint32_t(lastPresented);
  }
  if (ok) state.glass = lastKind;
  timing::outcome(ok ? lastKind.c_str() : "panel_fail");
  if (ok || settings.countRefreshes) saveState(state);
  return ok;
}

std::string App::webPlateUrl(const std::string &name) const {
  // <url>/<Scientific_name>.bin, "{region}" in the URL replaced by the
  // plates' region; spaces as underscores and anything else outside the
  // unreserved set percent-encoded, as export_web_plates.py names the files.
  std::string url = settings.webPlatesUrl;
  const size_t at = url.find("{region}");
  if (at != std::string::npos) url.replace(at, 8, packRegion);
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/";
  static const char *hex = "0123456789ABCDEF";
  for (unsigned char c : name) {
    if (c == ' ') {
      url += '_';
    } else if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
      url += char(c);
    } else {
      url += '%';
      url += hex[c >> 4];
      url += hex[c & 15];
    }
  }
  return url + ".bin";
}

uint32_t App::pageSignature() const {
  uint32_t h = 2166136261u;  // FNV-1a
  const auto add = [&h](const std::string &s) {
    for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
    h = (h ^ 0xffu) * 16777619u;  // a separator, so "ab","c" is not "a","bc"
  };
  const auto num = [&add](long v) { add(std::to_string(v)); };
  for (size_t i = 0; i < pageBirds.size(); ++i) {
    add(pageBirds[i]);
    add(i < pageCommon.size() ? pageCommon[i] : "");
  }
  // The date as it would be printed: a new day is a new page, a new hour is not.
  const std::time_t now = std::time(nullptr);
  if (settings.showDate && now > 100000) {
    std::tm tm{};
    localtime_r(&now, &tm);
    add(formatDate(tm, settings.dateStyle, settings.dateOrder));
  }
  add(refreshNote());  // counting refreshes changes the page every time, as it should
  add(kFirmwareVersion);
  add(settings.pack);
  add(settings.webPlates ? settings.webPlatesUrl : "");
  for (long v : {long(state.layout), long(settings.rotation), long(settings.names),
                 long(settings.commonCase), long(settings.labelSize), long(settings.sciPercent),
                 long(settings.packStyle), long(settings.showDate), long(settings.dateStyle),
                 long(settings.dateOrder), long(settings.dateEdge), long(settings.dateAlign),
                 long(settings.vivid), long(settings.sharpen), long(settings.edges),
                 long(settings.cream)})
    num(v);
  return h ? h : 1;  // 0 means "no page"
}

std::string App::refreshNote() const {
  if (!settings.countRefreshes) return "";
  return "Refresh " + std::to_string(state.refreshes + 1);
}

void App::resetRefreshCount() {
  state.refreshes = 0;
  const std::time_t now = std::time(nullptr);
  state.refreshesSince = now > 100000 ? uint32_t(now) : 0;
  saveState(state);
}

std::string App::localTime(std::time_t t) const {
  if (t < 100000) return "never";
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::string App::clockTime(std::time_t t) const {
  if (t < 100000) return "never";
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M", &tm);
  return buf;
}

uint64_t App::sleepSeconds(std::time_t now) const {
  uint64_t seconds = uint64_t(std::max(1, settings.intervalMin)) * 60;
  if (!inQuietHours(now)) return seconds;
  // Sleep straight through to the end of the quiet window.
  std::tm tm{};
  localtime_r(&now, &tm);
  int minutes = settings.quietTo - (tm.tm_hour * 60 + tm.tm_min);
  if (minutes <= 0) minutes += 24 * 60;
  return std::max<uint64_t>(60, uint64_t(minutes) * 60 - uint64_t(tm.tm_sec));
}

bool App::inQuietHours(std::time_t t) const {
  if (settings.quietFrom == settings.quietTo) return false;
  if (t < 100000) return false;  // no clock yet: never skip on a guess
  std::tm tm{};
  localtime_r(&t, &tm);
  const int m = tm.tm_hour * 60 + tm.tm_min;
  if (settings.quietFrom < settings.quietTo) return m >= settings.quietFrom && m < settings.quietTo;
  return m >= settings.quietFrom || m < settings.quietTo;  // wraps midnight
}

std::string App::apSsid() const {
  // The last two bytes of the MAC, so two frames in one house get two names.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // readable before the radio is up
  char buf[32];
  snprintf(buf, sizeof buf, "birdposter-%02X%02X", mac[4], mac[5]);
  return buf;
}

int App::batteryMv() {
  if (pins::kBatteryAdc < 0) return -1;
  // Behind a 1:2 divider that only conducts while the enable pin is high,
  // so the sense resistors do not drain the cell in sleep.
  pinMode(pins::kBatteryEnable, OUTPUT);
  digitalWrite(pins::kBatteryEnable, HIGH);
  delay(10);
  analogSetPinAttenuation(pins::kBatteryAdc, ADC_11db);
  const int mv = analogReadMilliVolts(pins::kBatteryAdc) * 2;
  digitalWrite(pins::kBatteryEnable, LOW);
  return mv;
}

std::vector<std::string> App::statusLines() {
  std::vector<std::string> lines;
  const bool sta = WiFi.status() == WL_CONNECTED;
  const bool ap = WiFi.getMode() & WIFI_MODE_AP;
  if (sta) {
    lines.push_back("WiFi: joined " + settings.wifiSsid + " as " +
                    std::string(WiFi.localIP().toString().c_str()) + " (" +
                    std::to_string(WiFi.RSSI()) + " dBm)");
    lines.push_back("Settings: http://" + std::string(WiFi.localIP().toString().c_str()) +
                    "/  or  http://" + settings.hostname + ".local/");
  } else if (ap) {
    lines.push_back("WiFi: own network " + apSsid() + ", password " + settings.apPass);
    lines.push_back("Settings: http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/");
  } else if (!settings.configured()) {
    lines.push_back("!WiFi: not set up - hold key 1 for the setup network");
  } else {
    lines.push_back("!WiFi: not joined to " + settings.wifiSsid + " (" +
                    std::to_string(state.wifiFailures) + " failures in a row)");
    lines.push_back("Settings: press key 1 to bring WiFi up");
  }

  const SourceConfig cfg = sourceConfig();
  char geo[80];
  snprintf(geo, sizeof geo, "%d km around %.4f, %.4f", settings.radiusKm, settings.lat, settings.lng);
  std::string window;
  switch (settings.lookbackUnit) {
    case Settings::Lookback::Minutes: window = std::to_string(settings.lookback) + " min"; break;
    case Settings::Lookback::Hours: window = std::to_string(settings.lookback) + " h"; break;
    case Settings::Lookback::Days: window = std::to_string(settings.lookback) + " days"; break;
    case Settings::Lookback::SinceLast: window = "since the last update"; break;
  }
  const std::string modeName =
      settings.mode == Mode::Rarest ? (settings.source == Source::eBird ? "notable" : "rarest")
                                    : "most seen";
  switch (settings.source) {
    case Source::iNaturalist:
      lines.push_back("Source: iNaturalist v" + std::to_string(settings.inatVersion) + ", " +
                      modeName + ", " + geo + ", last " + window);
      break;
    case Source::eBird:
    case Source::Ala:
      lines.push_back(std::string("Source: ") + sourceName(settings.source) + ", " + modeName +
                      ", " + geo + ", last " + window);
      break;
    case Source::BirdNet:
      lines.push_back("Source: BirdNET-Go at " + settings.detectorUrl + ", " + modeName +
                      ", last " + window);
      break;
    case Source::JsonList:
      lines.push_back("Source: JSON list at " + settings.listUrl + ", " + modeName);
      break;
  }
  const std::string url = requestUrl(cfg, std::time(nullptr), settings.mode);
  lines.push_back("Endpoint: " + url.substr(0, url.find('?')));
  if (!settings.sourceConfigured()) {
    const char *what = "no location yet - open the settings page and find a place";
    if (settings.source == Source::BirdNet) what = "no BirdNET-Go address yet - open the settings page";
    else if (settings.source == Source::JsonList) what = "no list URL yet - open the settings page";
    else if (settings.source == Source::eBird && settings.ebirdKey.empty())
      what = "no eBird API key yet - open the settings page";
    lines.push_back(std::string("!Set up: ") + what);
  }
  if (state.fetchOk)
    lines.push_back("Last fetch: ok, HTTP " + std::to_string(state.lastHttp));
  else
    lines.push_back("!Last fetch: " + (fetchError.empty() ? state.lastResult : fetchError));
  lines.push_back("Last page: " + (state.lastBirds.empty() ? std::string("none yet")
                                                            : state.lastBirds));
  lines.push_back("Rendered: " + localTime(state.lastRender) + ", layout " +
                  std::to_string(state.layout));
  char quiet[32];
  snprintf(quiet, sizeof quiet, "%02d:%02d-%02d:%02d", settings.quietFrom / 60, settings.quietFrom % 60,
           settings.quietTo / 60, settings.quietTo % 60);
  lines.push_back("Refresh: every " + std::to_string(settings.intervalMin) + " min, " +
                  (settings.quietFrom == settings.quietTo ? std::string("never quiet")
                                                          : "quiet " + std::string(quiet)) +
                  ", " + (everyBird() ? "every bird detected, up to " + std::to_string(settings.birds) : std::to_string(settings.birds) + " birds") + ", " +
                  (settings.portrait() ? "portrait" : "landscape"));
  {
    // What happens next, so the glass explains its own silence.
    const std::time_t now = std::time(nullptr);
    std::string next;
    if (portalForSetup) {
      next = "staying awake until set up";
    } else if (now < 100000) {
      next = "no clock yet";
    } else if (portalSleepAt) {
      next = "WiFi off at " + clockTime(portalSleepAt) + " unless touched, then a page about " +
             clockTime(portalSleepAt + std::time_t(sleepSeconds(portalSleepAt)));
    } else {
      next = "about " + clockTime(now + std::time_t(sleepSeconds(now)));
    }
    lines.push_back("Next: " + next);
  }
  lines.push_back("Plates: " + (platesOk ? std::to_string(plates.count()) + " species from " + packPath
                                         : "!" + platesError));
  lines.push_back("Clock: " + localTime(std::time(nullptr)) + " " + settings.tz);
  if (settings.webPlates)
    lines.push_back("Web plates: " + settings.webPlatesUrl +
                    (packRegion.empty() ? std::string("") : " (region " + packRegion + ")") +
                    (lastWebPlates.empty() ? "" : " - " + lastWebPlates));
  if (settings.countRefreshes)
    lines.push_back("Refreshes: " + std::to_string(state.refreshes + 1) + " including this one, since " +
                    (state.refreshesSince ? localTime(state.refreshesSince) : std::string("now")));
  if (const int mv = batteryMv(); mv >= 0) {
    char buf[48];
    snprintf(buf, sizeof buf, "Battery: %d.%02d V", mv / 1000, (mv % 1000) / 10);
    lines.push_back(buf);
  }
  lines.push_back("Memory: " + std::to_string(ESP.getFreeHeap() / 1024) + " KB heap, " +
                  std::to_string(ESP.getFreePsram() / 1024) + " KB PSRAM free");
  lines.push_back(std::string("Firmware: ") + kFirmwareVersion);
  lines.push_back("Keys: 1 WiFi on/off for setup, 2 this page on/off, 3 new page now");
  return lines;
}

}  // namespace birdposter
