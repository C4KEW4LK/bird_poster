// Where a wake's time goes, kept for the last few wakes.
//
// Each stage adds the milliseconds it took to the wake in progress; going to
// sleep files that wake in a ring held in RTC memory, which survives deep
// sleep (but not a power cut) and costs no flash writes. The web UI serves
// the ring as /api/timing, so a frame on a short interval can be asked why
// its cycle is longer than the interval.
//
// Stages are a two-level tree. The top level does not overlap, so with the
// boot it adds up to the wake, and whatever is left is "other". A stage with
// parts is timed whole as well as by part, and what its parts do not cover
// shows as its "rest" - so an unexplained cost shows where it is, rather than
// vanishing into a sum.
//
// Beside the times, a few numbers that explain them - how many birds, how
// many scales the layout tried, the signal, the battery - set once a wake,
// and left out of the JSON when nothing set them.
#pragma once

#include <cstdint>
#include <string>

namespace birdposter::timing {

enum Stage : uint8_t {
  // Top level.
  Settle,    // the pause at the start for a held key to read
  Begin,     // settings, filesystem, pack, fonts
  Wifi,      // joining
  Clock,     // NTP
  Fetch,     // asking the source, parsing the reply, choosing the birds
  WebPlate,  // full-size plates from the web
  Render,    // the whole page composition, web plates excluded
  Panel,     // waking the glass, sending it the page, its refresh, its sleep
  Portal,    // serving the web UI, less anything it set off that is timed here
  // Begin's parts.
  BeginSettings,  // settings and state from NVS
  BeginMount,     // the LittleFS mount
  BeginPack,      // the plate pack's index
  BeginFonts,     // both faces read into RAM
  // Fetch's parts.
  FetchHttp,   // connect, TLS, the request and the whole body
  FetchParse,  // the JSON
  // Render's parts.
  RenderMasks,     // silhouettes decoded for the packer, names measured
  RenderLayout,    // the scale search
  RenderGrow,      // birds into the gaps
  RenderDecode,    // the placed birds' pixels decoded
  RenderResample,  // scaled onto the canvas
  RenderDither,    // canvas to six inks
  RenderText,      // names, date, note
  RenderRedo,      // a render that ran out of memory and was thrown away for a redraw
  RenderWebDecode, // the full-size web plates decoded, from replies fetched before the draw
  // Panel's parts.
  PanelInit,      // power, reset and the init sequence
  PanelPush,      // the frame over SPI
  PanelPowerOn,   // the controllers' rails coming up
  PanelUpdate,    // the refresh proper: the ~30 s of flashing
  PanelPowerOff,  // the rails going down
  PanelSleep,     // the controllers into deep sleep
  kStages
};

enum Stat : uint8_t {
  CpuMhz,
  Birds,          // drawn on the page
  Attempts,       // scales the layout tried, each a whole packing
  WebPlates,      // full-size plates fetched
  FetchBytes,     // the source's reply
  RssiDbm,
  FastJoin,       // 1 when the remembered access point answered, 0 when it scanned
  BatteryMv,
  MinHeapKb,      // the least free internal heap since boot
  MinPsramKb,     // the least free PSRAM since boot
  // Inside render's layout and grow (PackCounters): how often, and how long.
  SpriteBuilds,   // a bird's mask built at a size: scaled, eroded, its name added
  SpriteBuildMs,
  PlaceSearches,  // the spiral's search of the page for one bird
  PlaceSearchMs,
  FitChecks,      // grow asking whether a bigger bird still fits
  FitCheckMs,
  LumaKept,       // birds drawn from the silhouette pass's plane, not decoded twice
  PsramLargestKb, // the largest free piece of PSRAM as the render starts
  SpriteScaleMs,  // sprite_build_ms's three parts
  SpriteErodeMs,
  SpriteLabelMs,
  DitherTableMs,    // render.dither's parts: the colour table,
  DitherLoadMs,     // canvas rows through it,
  DitherPrepareMs,  // boost, sharpen and edges a row at a time,
  DitherDiffuseMs,  // and the error diffusion
  DitherWaitMs,     // the diffusion waiting on the other core's preparing
  FastKb,           // fast-memory requests in the render the hook granted,
  FastFallbackKb,   // and the ones that fell back to PSRAM
  // Counted over the wake (bump), not the last page's (set):
  Pages,            // bird pages drawn or tried
  PageRedos,        // pages that ran out of memory and were redrawn with flash plates
  PagesOom,         // pages that ran out of memory even then, and were not drawn
  WebPlateOom,      // web plates that ran out of memory; that bird drawn from flash
  RadioOffAtMs,     // when the radio went off, ms from the chip's start: before the render, or for the refresh
  LayoutFallbacks,  // pages whose pack style could not fit the set, laid out as classic (counted)
  WifiAttempts,     // joins tried: remembered access point, scan, and the restart-and-retry
  WifiReason,       // the driver's last disconnect reason this wake (wifi_err_reason_t), if any
  kStats
};

// The wake starts: `by` is "timer", "key" or "power".
void start(const char *by);
void add(Stage s, uint32_t ms);
void set(Stat s, int32_t value);
// Add to a count kept over the wake.
void bump(Stat s, int32_t by = 1);

// One bird page, for the list of the wake's pages in json(): what it drew
// and whether memory got in its way. Kept in RAM for the wake in progress
// only - the RTC ring keeps the wake's totals (Pages, PageRedos...).
struct PageRecord {
  int birds = 0;
  int webFetched = 0;  // full-size plates drawn from the web
  int webOom = 0;      // ... that ran out of memory, drawn from flash instead
  bool redone = false; // the render ran out of memory and was redrawn, flash plates only
  bool drawn = false;  // false: out of memory even then, or failed
  bool fellBack = false;  // the pack style could not fit the set; laid out as classic
  uint32_t renderMs = 0, redoMs = 0;
  int32_t psramLargestKb = 0;  // the largest free piece as it started
  char layout[24] = {0};       // "hero 6 names": pack style, birds asked for, names on or off
};
void page(const PageRecord &p);
// What the page came to: "birds", "status", "setup", "unchanged", "failed".
void outcome(const char *what);
// The top-level stages so far, summed: what a stage that may set off others
// (the portal) takes off its own wall time.
uint32_t accounted();
// The wake ends, about to sleep this long: file it in the ring.
void finish(uint64_t sleepSeconds);
// One line for the serial log: the wake so far.
std::string summary();
// The ring, newest first, and the wake in progress, as JSON.
std::string json();

// Times the scope it lives in into `s`.
class Scope {
 public:
  explicit Scope(Stage s);
  ~Scope();
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

 private:
  Stage stage_;
  uint32_t at_;
};

}  // namespace birdposter::timing
