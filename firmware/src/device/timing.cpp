#include "timing.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <sys/time.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace birdposter::timing {

namespace {

struct StageInfo {
  const char *name;
  int8_t parent;  // a Stage, or -1 at the top level
};

// In Stage order. A part names the stage it is part of.
const StageInfo kStageInfo[] = {
    {"settle", -1},     {"begin", -1},        {"wifi", -1},      {"clock", -1},
    {"fetch", -1},      {"web_plate", -1},    {"render", -1},    {"panel", -1},
    {"portal", -1},     {"settings", Begin},  {"mount", Begin},  {"pack", Begin},
    {"fonts", Begin},   {"http", Fetch},      {"parse", Fetch},  {"masks", Render},
    {"layout", Render}, {"grow", Render},     {"decode", Render}, {"resample", Render},
    {"dither", Render}, {"text", Render},     {"redo", Render},  {"web_decode", Render},  {"init", Panel},   {"push", Panel},
    {"power_on", Panel}, {"update", Panel},   {"power_off", Panel}, {"sleep", Panel},
};
static_assert(sizeof kStageInfo / sizeof kStageInfo[0] == kStages, "a Stage without a name");

const char *const kStatNames[] = {"cpu_mhz",  "birds",      "layout_attempts", "web_plates",
                                        "fetch_bytes", "rssi_dbm", "fast_join",       "battery_mv",
                                        "min_heap_kb", "min_psram_kb", "sprite_builds",
                                        "sprite_build_ms", "place_searches", "place_search_ms",
                                        "fit_checks",  "fit_check_ms", "luma_kept",
                                        "psram_largest_kb", "sprite_scale_ms",
                                        "sprite_erode_ms", "sprite_label_ms", "dither_table_ms",
                                        "dither_load_ms", "dither_prepare_ms", "dither_diffuse_ms", "dither_wait_ms",
                                        "fast_kb", "fast_fallback_kb", "pages", "page_redos",
                                        "pages_oom", "web_plate_oom", "radio_off_at_ms",
                                        "layout_fallbacks", "wifi_attempts", "wifi_reason"};
// One name a Stat, in order: a short list compiled with null names, and the
// JSON writer then crashed on them.
static_assert(sizeof kStatNames / sizeof kStatNames[0] == kStats, "a Stat without a name");

constexpr int32_t kUnset = INT32_MIN;

// Plain data, no initialisers: RTC memory must not be touched by a
// constructor on wake, or the ring would be cleared every time.
struct Wake {
  int64_t startMs;   // wall clock at the chip's start, 0 for no clock
  char by[8];
  char outcome[12];
  uint8_t reset;     // esp_reset_reason_t
  uint32_t bootMs;   // from the chip's start to setup()
  uint32_t awakeMs;  // from the chip's start to sleep
  uint32_t sleepS;
  uint32_t ms[kStages];
  int32_t stat[kStats];
};

constexpr int kKept = 16;
RTC_DATA_ATTR Wake ring[kKept];
RTC_DATA_ATTR uint32_t filed = 0;  // wakes filed since power-on; the newest is ring[(filed-1) % kKept]

Wake now;

const char *resetName(uint8_t r) {
  switch (esp_reset_reason_t(r)) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";  // the flasher's reset, over the USB port
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

int64_t wallMs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 100000) return 0;
  return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

uint32_t topSum(const Wake &w) {
  uint32_t sum = 0;
  for (int s = 0; s < kStages; ++s)
    if (kStageInfo[s].parent < 0) sum += w.ms[s];
  return sum;
}

uint32_t other(const Wake &w) {
  const uint32_t sum = w.bootMs + topSum(w);
  return w.awakeMs > sum ? w.awakeMs - sum : 0;
}

bool hasParts(int stage) {
  for (int s = 0; s < kStages; ++s)
    if (kStageInfo[s].parent == stage) return true;
  return false;
}

std::string stamp(int64_t ms) {
  if (ms <= 0) return "";
  const std::time_t t = std::time_t(ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[24];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

std::string wakeJson(const Wake &w, int64_t prevStartMs) {
  char buf[96];
  std::string j = "{\"at\":\"" + stamp(w.startMs) + "\",\"by\":\"" + w.by + "\",\"reset\":\"" +
                  resetName(w.reset) + "\",\"outcome\":\"" + w.outcome + "\"";
  snprintf(buf, sizeof buf, ",\"awake_ms\":%lu,\"sleep_s\":%lu", (unsigned long)w.awakeMs,
           (unsigned long)w.sleepS);
  j += buf;
  // The cycle as it really was: this wake's start to the next one's.
  if (prevStartMs > 0 && w.startMs > 0) {
    snprintf(buf, sizeof buf, ",\"cycle_s\":%.1f", double(prevStartMs - w.startMs) / 1000.0);
    j += buf;
  }
  snprintf(buf, sizeof buf, ",\"ms\":{\"boot\":%lu", (unsigned long)w.bootMs);
  j += buf;
  for (int s = 0; s < kStages; ++s) {
    if (kStageInfo[s].parent >= 0) continue;
    if (!hasParts(s)) {
      snprintf(buf, sizeof buf, ",\"%s\":%lu", kStageInfo[s].name, (unsigned long)w.ms[s]);
      j += buf;
      continue;
    }
    // {"total": the stage timed whole, its parts, and "rest": what they miss}
    snprintf(buf, sizeof buf, ",\"%s\":{\"total\":%lu", kStageInfo[s].name, (unsigned long)w.ms[s]);
    j += buf;
    uint32_t parts = 0;
    for (int p = 0; p < kStages; ++p) {
      if (kStageInfo[p].parent != s) continue;
      parts += w.ms[p];
      snprintf(buf, sizeof buf, ",\"%s\":%lu", kStageInfo[p].name, (unsigned long)w.ms[p]);
      j += buf;
    }
    snprintf(buf, sizeof buf, ",\"rest\":%lu}",
             (unsigned long)(w.ms[s] > parts ? w.ms[s] - parts : 0));
    j += buf;
  }
  snprintf(buf, sizeof buf, ",\"other\":%lu},\"stats\":{", (unsigned long)other(w));
  j += buf;
  bool first = true;
  for (int s = 0; s < kStats; ++s) {
    if (w.stat[s] == kUnset) continue;
    snprintf(buf, sizeof buf, "%s\"%s\":%ld", first ? "" : ",", kStatNames[s], (long)w.stat[s]);
    j += buf;
    first = false;
  }
  return j + "}}";
}

}  // namespace

void start(const char *by) {
  now = Wake{};
  now.bootMs = millis();
  const int64_t wall = wallMs();
  now.startMs = wall > 0 ? wall - now.bootMs : 0;
  std::strncpy(now.by, by, sizeof now.by - 1);
  now.reset = uint8_t(esp_reset_reason());
  for (int32_t &v : now.stat) v = kUnset;
  now.stat[CpuMhz] = int32_t(getCpuFrequencyMhz());
}

void add(Stage s, uint32_t ms) { now.ms[s] += ms; }

void set(Stat s, int32_t value) { now.stat[s] = value; }

void bump(Stat s, int32_t by) { now.stat[s] = (now.stat[s] == kUnset ? 0 : now.stat[s]) + by; }

namespace {
struct PageAt {
  PageRecord p;
  int64_t wallMs;
  uint32_t uptimeMs;
};
constexpr size_t kPagesKept = 32;
std::vector<PageAt> pages;  // this wake's, oldest first
}  // namespace

void page(const PageRecord &p) {
  if (pages.size() == kPagesKept) pages.erase(pages.begin());
  pages.push_back({p, wallMs(), millis()});
  bump(Pages);
  if (p.redone) bump(PageRedos);
  if (!p.drawn) bump(PagesOom);
  if (p.webOom) bump(WebPlateOom, p.webOom);
  if (p.fellBack) bump(LayoutFallbacks);
}

void outcome(const char *what) {
  std::memset(now.outcome, 0, sizeof now.outcome);
  std::strncpy(now.outcome, what, sizeof now.outcome - 1);
}

uint32_t accounted() { return topSum(now); }

void finish(uint64_t sleepSeconds) {
  now.awakeMs = millis();
  now.sleepS = uint32_t(sleepSeconds);
  now.stat[MinHeapKb] = int32_t(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
  if (psramFound())
    now.stat[MinPsramKb] = int32_t(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
  // No clock at the start (a cold boot), but one now: date it from here.
  if (now.startMs == 0) {
    const int64_t wall = wallMs();
    if (wall > 0) now.startMs = wall - now.awakeMs;
  }
  ring[filed % kKept] = now;
  ++filed;
}

std::string summary() {
  Wake w = now;
  w.awakeMs = millis();
  char buf[48];
  snprintf(buf, sizeof buf, "wake %lu ms: boot %lu", (unsigned long)w.awakeMs,
           (unsigned long)w.bootMs);
  std::string s = buf;
  for (int t = 0; t < kStages; ++t) {
    if (kStageInfo[t].parent >= 0 || !w.ms[t]) continue;
    snprintf(buf, sizeof buf, ", %s %lu", kStageInfo[t].name, (unsigned long)w.ms[t]);
    s += buf;
    // The parts in brackets: "render 4210 (layout 2100, dither 1300, ...)".
    bool open = false;
    for (int p = 0; p < kStages; ++p) {
      if (kStageInfo[p].parent != t || !w.ms[p]) continue;
      snprintf(buf, sizeof buf, "%s%s %lu", open ? ", " : " (", kStageInfo[p].name,
               (unsigned long)w.ms[p]);
      s += buf;
      open = true;
    }
    if (open) s += ")";
  }
  snprintf(buf, sizeof buf, ", other %lu", (unsigned long)other(w));
  return s + buf;
}

std::string json() {
  Wake cur = now;
  cur.awakeMs = millis();
  std::string j = "{\"now\":\"" + stamp(wallMs()) + "\",\"current\":" + wakeJson(cur, 0);
  // This wake's bird pages, newest first.
  j += ",\"pages\":[";
  for (size_t i = 0; i < pages.size(); ++i) {
    const PageAt &a = pages[pages.size() - 1 - i];
    char buf[320];
    snprintf(buf, sizeof buf,
             "%s{\"at\":\"%s\",\"uptime_s\":%lu,\"layout\":\"%s\",\"birds\":%d,\"web_plates\":%d,\"web_plate_oom\":%d,"
             "\"redone\":%s,\"drawn\":%s,\"fell_back\":%s,\"render_ms\":%lu,\"redo_ms\":%lu,\"psram_largest_kb\":%ld}",
             i ? "," : "", stamp(a.wallMs).c_str(), (unsigned long)(a.uptimeMs / 1000), a.p.layout, a.p.birds,
             a.p.webFetched, a.p.webOom, a.p.redone ? "true" : "false", a.p.drawn ? "true" : "false", a.p.fellBack ? "true" : "false",
             (unsigned long)a.p.renderMs, (unsigned long)a.p.redoMs, (long)a.p.psramLargestKb);
    j += buf;
  }
  j += "],\"wakes\":[";
  const uint32_t n = filed < uint32_t(kKept) ? filed : uint32_t(kKept);
  int64_t next = cur.startMs;
  for (uint32_t i = 0; i < n; ++i) {
    const Wake &w = ring[(filed - 1 - i) % kKept];
    if (i) j += ",";
    j += wakeJson(w, next);
    next = w.startMs;
  }
  return j + "]}";
}

Scope::Scope(Stage s) : stage_(s), at_(millis()) {}
Scope::~Scope() { add(stage_, millis() - at_); }

}  // namespace birdposter::timing
