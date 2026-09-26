// What the owner can change, and what the frame remembers between sleeps.
//
// Both live in NVS through Preferences: `Settings` under "frame", written by
// the web UI; `State` under "state", written by the frame itself on every
// cycle. Kept apart so a settings save never clobbers the layout counter and a
// factory reset of one need not touch the other.
#pragma once

#include <cstdint>
#include <string>

#include "pages.h"
#include "source.h"

namespace birdframe {

struct Settings {
  // The network the frame joins. Empty SSID means "not set up yet", which is
  // what puts the frame into its own access point with the setup page.
  std::string wifiSsid;
  std::string wifiPass;
  std::string apPass = "birdframe";  // the setup network's own password
  std::string hostname = "birdframe";  // also the mDNS name: http://birdframe.local/

  Source source = Source::iNaturalist;
  Mode mode = Mode::MostDetected;
  std::string detectorUrl = "http://birdnet-go.local:8080";
  std::string ebirdKey;  // from https://ebird.org/api/keygen; the frame only reads
  std::string ebirdLocale = "en_AU";  // which English the common names come in
  std::string listUrl;   // a JSON list of names the owner serves
  double lat = 0.0;
  double lng = 0.0;
  int radiusKm = 25;
  // How far back the page looks. Minutes, hours or days of `lookback`, or
  // since the last page was drawn.
  enum class Lookback : uint8_t { Minutes = 0, Hours = 1, Days = 2, SinceLast = 3 };
  int lookback = 30;
  Lookback lookbackUnit = Lookback::Days;
  int inatVersion = 2;  // iNaturalist API: 2 lean but may change, 1 frozen but 8x the bytes

  int birds = 10;       // on the page; the packer copes with up to 40
  int rotation = 1;     // quarter turns from the panel's portrait; 1 = landscape
  NameStyle names = NameStyle::Both;  // what goes under each bird
  NameCase commonCase = NameCase::Upper;  // how the common name is cased
  LabelSize labelSize = LabelSize::Medium;
  int sciPercent = 70;  // scientific name under a common one, % of its size
  PackStyle packStyle = PackStyle::Classic;  // how the birds are arranged on the page
  bool showDate = false;  // today's date along one edge of the bird page
  bool countRefreshes = false;  // count every refresh of the glass, for a battery test
  // Full-size plates from the web, for a bird drawn much larger than its
  // plate in flash; the flash plate whenever the site does not answer. The
  // files sit under the URL as <Scientific_name>.bin; a "{region}" in it is
  // replaced by the plates' region ("au"), for a site that hosts several.
  // The default is the project's GitHub Pages site, where the flasher's own
  // build publishes them (webflash.py); a private copy elsewhere is entered in
  // the web UI.
  bool webPlates = true;
  std::string webPlatesUrl = "https://c4kew4lk.github.io/bird_poster/plates/{region}";
  DateStyle dateStyle = DateStyle::WordsLong;
  DateOrder dateOrder = DateOrder::DayFirst;  // for the numeric styles
  DateEdge dateEdge = DateEdge::Bottom;
  DateAlign dateAlign = DateAlign::Right;
  int vivid = 2;    // colour boost before the dither, 0 least .. 4 most
  int sharpen = 2;  // unsharp mask before the dither, 0 off .. 4 most
  int edges = 2;    // ink along detected edges, 0 off .. 4 strongest
  int cream = 0;    // the page printed on cream rather than white, 0 off .. 4 warmest
  int cycleHours = 0;  // rotate: no bird twice within this many hours; 0 draws the top birds every time

  // Which plate pack to draw from, where the filesystem holds more than one:
  // "au", "eu", "us" for /plates-<pack>.bin. Empty means /plates.bin, the one
  // pack a 16 MB board carries.
  std::string pack;

  int intervalMin = 60;  // between refreshes
  // Minutes after midnight, local time.
  int quietFrom = 0;     // no refreshes from this time...
  int quietTo = 6 * 60;  // ...to this one; equal means never quiet
  // POSIX, for the quiet hours and the status page. Sydney/Canberra/Melbourne
  // with daylight saving; plain "AEST-10" is Brisbane.
  std::string tz = "AEST-10AEDT,M10.1.0,M4.1.0/3";

  bool portrait() const { return (rotation & 1) == 0; }
  bool configured() const { return !wifiSsid.empty(); }
  // Whether the source can be asked for anything: the place-based sources
  // need a place, eBird a key besides, BirdNET-Go an address, a list its URL.
  // Until it can, the frame stays awake for setup.
  bool sourceConfigured() const {
    switch (source) {
      case Source::BirdNet: return !detectorUrl.empty();
      case Source::JsonList: return !listUrl.empty();
      case Source::eBird: return !ebirdKey.empty() && !(lat == 0.0 && lng == 0.0);
      case Source::iNaturalist:
      case Source::Ala: return !(lat == 0.0 && lng == 0.0);
    }
    return false;
  }
  // Whether the source is asked about a latitude and longitude at all.
  bool placeBased() const {
    return source == Source::iNaturalist || source == Source::eBird || source == Source::Ala;
  }
};

struct State {
  int layout = 0;           // which arrangement the page is on
  int wifiFailures = 0;     // consecutive, since the last good join
  uint32_t lastRender = 0;  // epoch seconds, 0 for never
  std::string lastResult;   // one line: what the last cycle did
  std::string lastBirds;    // names on the page, comma separated
  bool fetchOk = false;
  int lastHttp = 0;
  bool showingStatus = false;  // key 2 toggles this
  bool portalOn = false;       // key 1 sets this; the web UI or the idle timeout clears it
  std::string glass;           // what is on the panel: birds, status, setup, pattern
  uint32_t refreshes = 0;      // glass refreshes counted, while counting is on
  uint32_t refreshesSince = 0; // epoch seconds the count started, 0 for not yet
};

void loadSettings(Settings &s);
void saveSettings(const Settings &s);
void loadState(State &s);
void saveState(const State &s);

}  // namespace birdframe
