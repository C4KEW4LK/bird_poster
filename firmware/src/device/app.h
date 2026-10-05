// The frame's working parts, shared between the boot sequence, the keys and
// the web UI: the pack, the font, the panel, the last page drawn and the
// functions that turn a setting into a picture.
#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pages.h"
#include "panel.h"
#include "plates.h"
#include "render.h"
#include "settings.h"
#include "source.h"

namespace birdposter {

constexpr const char *kPlatesPath = "/plates.bin";  // a board with one pack
constexpr const char *kPackPrefix = "/plates-";      // "/plates-au.bin": a board with several
constexpr const char *kFontPath = "/label.ttf";  // the scientific name, and everything else
constexpr const char *kNameFontPath = "/name.ttf";  // the common name; optional
// When each species was last on the glass, for the cycle setting: "epoch name"
// a line. On the plates filesystem rather than NVS because it can run to a few
// hundred lines, and losing it to a re-flash of the plates costs nothing.
constexpr const char *kShownPath = "/shown.txt";
constexpr const char *kRegionPath = "/region.txt";  // which region a lone /plates.bin is
extern const char *kFirmwareVersion;  // the commit the build came from

struct App {
  Settings settings;
  State state;
  Plates plates;
  bool platesOk = false;
  std::string platesError;
  std::string packPath;               // the pack file actually mounted
  std::string packKey;                // its region key ("au"), or "" for a lone /plates.bin
  std::string packRegion;             // the region the plates are, for the web: packKey, or /region.txt
  std::string lastWebPlates;          // what the web supplement did on the last bird page
  bool renderOutOfMemory = false;     // the last showBirds failed for want of PSRAM
  bool packOnCard = false;            // read from the SD card rather than the flash filesystem
  std::vector<std::string> packs;     // packs available, by region key, card and flash together
  std::vector<std::string> cardPacks; // the subset that is on the card
  // Mount the pack the settings name (or the only one), replacing what is open.
  // A pack on the SD card wins over the same region in flash: the card holds
  // the plates at their full size, the flash a copy shrunk to fit.
  bool openPack();
  // The cell's voltage, or -1 on a board that cannot read it.
  int batteryMv();
  // The SD slot, on a board that has one. The card is powered only while the
  // frame is awake and is mounted on demand; a missing or unreadable card is
  // not an error, the flash pack is what it falls back to.
  bool mountCard();
  void unmountCard();
  bool cardOk = false;
  std::string cardError;
  Font font;
  bool fontOk = false;
  Font nameFont;  // for the common name; falls back to `font` when absent
  Panel panel;

  // The page most recently composed, kept for the web UI's preview. Its
  // pixels are `pageMemory`'s when that was reserved.
  Frame last;
  // One block for every page, reserved first thing in begin() - before the
  // fonts, the pack's index and WiFi have touched PSRAM - and kept for the
  // wake: a 1600 x 1200 RGB565 canvas, which the dither turns into the frame
  // in place. Null if PSRAM could not give it, and pages allocate as they go.
  uint16_t *pageMemory = nullptr;
  size_t pageMemoryPx = 0;
  // Point `last` at pageMemory, empty, so the next page is drawn there.
  void pageIntoReserve();
  std::string lastKind;  // "birds", "status", "setup", "pattern" or ""
  std::time_t lastPresented = 0;  // when it went to the glass; the preview's cache key

  // While the portal is up: when it closes if nobody touches it. 0 otherwise.
  std::time_t portalSleepAt = 0;
  // The portal is staying up until the frame is set up: no network, or a
  // network but no source to ask. No idle timeout applies.
  bool portalForSetup = false;

  // What the last fetch said, for the status page and the UI.
  std::string fetchError;
  std::vector<std::string> pageBirds;   // scientific, the artwork key
  std::vector<std::string> pageCommon;  // parallel; may be empty strings

  // What the frame is doing right now, one short line, empty when idle. The
  // web UI shows it while a page is being made. Every stage calls
  // `progress`, which also runs `onProgress` - main.cpp hangs the web
  // server's poll on it, so the settings page stays answerable through a
  // fetch, a render and the 30 s panel refresh, all of which block.
  std::string phase;
  std::function<void()> onProgress;
  // Run once, just before the next page goes to the glass, then cleared.
  // main.cpp sets it to turn the radio off when nothing after the refresh
  // needs the network, so WiFi is not up through the 30 s the glass takes.
  std::function<void()> beforeRefresh;
  // Run once by the next bird page, after its layout and its web plates are
  // fetched and before any drawing: the render needs no network from there,
  // so a wake that turns the radio off for the refresh turns it off here
  // instead. Cleared once run. Null leaves the radio as it is.
  std::function<void()> beforeRender;
  void progress(const std::string &what);

  bool begin();  // filesystem, pack, font, settings, state

  // GET a URL into `body`. HTTPS is accepted without a CA (see the notes: the
  // frame has no clock at first boot and no room for a root store), and the
  // status code comes back in `http`. False on transport failure, with `error`
  // saying why.
  bool fetch(const std::string &url, std::string &body, int &http, std::string &error,
             const std::vector<std::pair<std::string, std::string>> &headers = {},
             uint32_t timeoutMs = 20000);

  // Ask a source for its species list. Needs WiFi. `http` is the status code,
  // or 0 when nothing answered. False with `error` set; `seen` is then empty.
  bool querySource(const SourceConfig &cfg, Mode mode, std::vector<Sighting> &seen, int &http,
                   std::string &error);

  // The frame's own settings as a source config.
  SourceConfig sourceConfig() const;
  // The window's start for these settings, or 0 for none; and the same as
  // BirdNET-Go's local "YYYY-MM-DD HH:MM:SS".
  std::time_t windowStart(int lookback, Settings::Lookback unit) const;
  std::string localStamp(std::time_t t) const;

  // Ask the source for birds and pick the page. Needs WiFi. False leaves the
  // previous page alone, with `fetchError` saying why.
  bool fetchBirds(std::vector<int> &plateIndices);

  // Networks in range, from the last scan, strongest first and one entry a
  // name. Scanned on the first settings page while unjoined, and on request.
  struct Network {
    std::string ssid;
    int rssi = 0;
    bool secure = true;
  };
  std::vector<Network> networks;
  bool scanned = false;
  void scanNetworks();

  // The cycle's memory: when each species was last drawn.
  std::map<std::string, std::time_t> shown;
  void loadShown();
  void recordShown(const std::vector<std::string> &names);  // the page just drawn

  // What the settings page's Test button reports: the same request the next
  // refresh would make, against settings that may not be saved yet, without
  // touching the page or the fetch state.
  struct Probe {
    bool ok = false;
    int http = 0;
    std::string url;
    std::string error;
    size_t seen = 0;      // species the source reported
    size_t drawable = 0;  // of those, with a plate in the pack
    std::vector<std::string> sample;  // the first few drawable names
  };
  Probe probe(const SourceConfig &cfg, Mode mode);

  // Compose and show. Each returns false when the panel or the render failed.
  bool showBirds(const std::vector<int> &plateIndices);
  // False: showBirds renders the page but does not send it to the glass -
  // the stress test, which wants the render's memory and not 30 s of panel.
  bool presentPages = true;
#ifdef BIRDPOSTER_DEBUG
  // The memory stress test: fetch the species list once, then render `pages`
  // pages without the glass, cycling through layouts chosen for memory -
  // 2 to 40 birds, every pack style, names on and off, a new layout each -
  // with the frame's own web plates setting. Each page lands in /api/timing's
  // `pages` with its layout. The settings are as they were afterwards.
  void stressTest(int pages);
#endif
  bool showStatus();
  bool showSetup(const std::string &ssid, const std::string &pass, const std::string &url,
                 const std::string &note = "");
  bool showPattern();  // six colour bars, for proving the glass

  // Push `last` to the panel and refresh it.
  bool present();

  std::vector<std::string> statusLines();
  std::string localTime(std::time_t t) const;
  // "Refresh 123", counting the refresh about to happen, for the page and the
  // status page; empty while counting is off.
  std::string refreshNote() const;
  // A hash of everything that decides how the next bird page looks - the
  // birds and their names, the date, the layout, every drawing setting, the
  // firmware. Equal to State::pageSig when that page is already on the glass.
  uint32_t pageSignature() const;
  // Where the web supplement asks for a species' full-size plate.
  std::string webPlateUrl(const std::string &name) const;
  // Start the refresh count again from nothing.
  void resetRefreshCount();
  std::string clockTime(std::time_t t) const;  // HH:MM, or "never"
  bool inQuietHours() const { return inQuietHours(std::time(nullptr)); }
  // The page holds every species in the window rather than a set number.
  bool everyBird() const { return settings.everyBird && settings.source == Source::BirdNet; }
  bool inQuietHours(std::time_t t) const;
  // How long the next deep sleep lasts from `now`: the refresh interval, or
  // through to the end of the quiet window.
  uint64_t sleepSeconds(std::time_t now) const;
  std::string apSsid() const;
};

}  // namespace birdposter
