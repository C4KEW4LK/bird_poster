// The frame.
//
// Wake, decide what the wake was for, do that one thing, sleep. The timer
// wake is the ordinary cycle - join WiFi, ask the source, pack a page, push it
// - and the three keys are the exceptions:
//
//   key 1  turns the settings portal on: WiFi stays up and the web UI answers
//          until "Done" is clicked there or nobody has touched it for a
//          while. Pressed again, it only restarts that while. Without a network configured this is the frame's own access
//          point with the setup page on the glass.
//   key 2  toggles the status page: address, source, whether the endpoint
//          answered, the settings in force. Stays until pressed again.
//   key 3  a new page now: advances the layout and runs the ordinary cycle.
//
// Everything the cycle learns is written to NVS before sleeping, because deep
// sleep is a reboot: the next wake starts from `State`, not from RAM.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <algorithm>
#include <cstring>
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "app.h"
#include "corehelper.h"
#include "fastmem.h"
#include "timing.h"
#include "webui.h"

using namespace birdposter;

// The progress hook nests the web server's poll inside a fetch or a render,
// and the request handlers build a page on top of that. Must be at file scope:
// the core finds this function by name.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {

App app;
WebUi ui(app);

constexpr uint32_t kJoinTimeoutMs = 20000;
constexpr uint32_t kFastJoinMs = 4000;  // straight to the remembered access point, before a scan
constexpr uint32_t kRetryJoinMs = 12000;  // the one more try, after starting WiFi over
constexpr uint32_t kNtpTimeoutMs = 8000;
constexpr uint32_t kPortalIdleMs = 30 * 60 * 1000;  // untouched this long: sleep
constexpr int kFailuresBeforeAp = 3;  // joins in a row that fail before the AP comes up
constexpr const char *kApUrl = "http://192.168.4.1/";

enum class Key { None, One, Two, Three };

Key keyFromWake() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return Key::None;
  const uint64_t bits = esp_sleep_get_ext1_wakeup_status();
  if (bits & (1ULL << pins::kKey1)) return Key::One;
  if (bits & (1ULL << pins::kKey2)) return Key::Two;
  if (bits & (1ULL << pins::kKey3)) return Key::Three;
  return Key::None;
}

// A key held right now, debounced and waited out, so one press is one action.
Key keyPressed() {
  const int pin[] = {pins::kKey1, pins::kKey2, pins::kKey3};
  const Key key[] = {Key::One, Key::Two, Key::Three};
  for (int i = 0; i < 3; ++i) {
    if (digitalRead(pin[i]) != LOW) continue;
    delay(30);
    if (digitalRead(pin[i]) != LOW) continue;
    while (digitalRead(pin[i]) == LOW) delay(10);
    delay(30);
    return key[i];
  }
  return Key::None;
}

// The access point the last join landed on, kept in RTC memory: it survives
// deep sleep but not a power cut, and costs no flash writes. Joining with the
// channel and BSSID given skips the scan of every channel, which is most of
// the time a join takes. `rtcKey` ties it to the network it was learnt on.
RTC_DATA_ATTR uint8_t rtcBssid[6];
RTC_DATA_ATTR int32_t rtcChannel = 0;
RTC_DATA_ATTR uint32_t rtcKey = 0;

uint32_t networkKey() {
  uint32_t h = 2166136261u;  // FNV-1a over ssid, NUL, password
  for (char c : app.settings.wifiSsid) h = (h ^ uint8_t(c)) * 16777619u;
  h *= 16777619u;
  for (char c : app.settings.wifiPass) h = (h ^ uint8_t(c)) * 16777619u;
  return h;
}

bool waitJoined(uint32_t ms) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < ms) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Why the station last dropped or failed to join, as the WiFi driver says it
// (wifi_err_reason_t: 201 no access point found, 15 / 204 handshake timeout,
// 202 authentication failed...). 0 for none this wake.
volatile uint8_t wifiReason = 0;

// "no AP found (201)", or "" when the driver gave no reason.
std::string wifiReasonText() {
  if (!wifiReason) return "";
  return std::string(WiFi.disconnectReasonName(wifi_err_reason_t(wifiReason))) + " (" +
         std::to_string(wifiReason) + ")";
}

bool joinWifi() {
  if (!app.settings.configured()) return false;
  static bool listening = false;
  if (!listening) {
    WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
          // "Left" is this code disconnecting on purpose, between tries: it
          // would hide the reason the try before it failed.
          if (info.wifi_sta_disconnected.reason != WIFI_REASON_ASSOC_LEAVE)
            wifiReason = info.wifi_sta_disconnected.reason;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    listening = true;
  }
  wifiReason = 0;
  int attempts = 0;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(app.settings.hostname.c_str());
  const char *ssid = app.settings.wifiSsid.c_str(), *pass = app.settings.wifiPass.c_str();
  const uint32_t start = millis();
  const uint32_t key = networkKey();
  bool joined = false;
  if (rtcChannel > 0 && rtcKey == key) {
    Serial.printf("wifi: joining %s on channel %d\n", ssid, int(rtcChannel));
    ++attempts;
    WiFi.begin(ssid, pass, rtcChannel, rtcBssid);
    joined = waitJoined(kFastJoinMs);
    timing::set(timing::FastJoin, joined ? 1 : 0);
    if (!joined) {
      // Moved channel, a different access point, or gone: forget it and scan.
      Serial.println("wifi: remembered access point did not answer, scanning");
      rtcChannel = 0;
      WiFi.disconnect();
      delay(100);
    }
  }
  if (!joined) {
    Serial.printf("wifi: joining %s\n", ssid);
    ++attempts;
    WiFi.begin(ssid, pass);
    // What is left of the usual budget, but never under five seconds for the scan.
    const uint32_t spent = uint32_t(millis() - start);
    joined = waitJoined(kJoinTimeoutMs - std::min<uint32_t>(kJoinTimeoutMs - 5000, spent));
  }
  if (!joined) {
    // Seen on the first boot after a flash or a reset, more than once: the
    // join fails and the next wake's succeeds. Start the WiFi stack over -
    // off, a moment, on - and try once more before giving the wake up.
    Serial.printf("wifi: did not join%s%s; restarting WiFi for one more try\n",
                  wifiReason ? ": " : "", wifiReasonText().c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(app.settings.hostname.c_str());
    ++attempts;
    WiFi.begin(ssid, pass);
    joined = waitJoined(kRetryJoinMs);
  }
  timing::set(timing::WifiAttempts, attempts);
  if (wifiReason) timing::set(timing::WifiReason, wifiReason);
  if (!joined) {
    timing::add(timing::Wifi, millis() - start);
    Serial.printf("wifi: failed%s%s\n", wifiReason ? ": " : "", wifiReasonText().c_str());
    return false;
  }
  memcpy(rtcBssid, WiFi.BSSID(), sizeof rtcBssid);
  rtcChannel = WiFi.channel();
  rtcKey = key;
  Serial.printf("wifi: %s in %lu ms\n", WiFi.localIP().toString().c_str(),
                (unsigned long)(millis() - start));
  timing::add(timing::Wifi, millis() - start);
  timing::set(timing::RssiDbm, WiFi.RSSI());
  MDNS.begin(app.settings.hostname.c_str());
  MDNS.addService("http", "tcp", 80);
  return true;
}

// The radio off for good this wake. Called just before the glass refresh
// when the frame will sleep straight after it: nothing past that point
// needs the network, and the refresh is the longest thing a wake does.
void radioOff() {
  if (WiFi.getMode() == WIFI_OFF) return;  // off already: for the render
  timing::set(timing::RadioOffAtMs, int32_t(millis()));
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("wifi: off");
}

void syncClock() {
  configTzTime(app.settings.tz.c_str(), "pool.ntp.org", "time.google.com");
  const uint32_t start = millis();
  while (std::time(nullptr) < 100000 && millis() - start < kNtpTimeoutMs) delay(100);
  timing::add(timing::Clock, millis() - start);
  Serial.printf("clock: %s\n", app.localTime(std::time(nullptr)).c_str());
}

void startAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);  // the station side is what a network scan needs
  WiFi.softAP(app.apSsid().c_str(), app.settings.apPass.c_str());
  delay(100);
  Serial.printf("ap: %s at %s\n", app.apSsid().c_str(), WiFi.softAPIP().toString().c_str());
}

// The switches the frame drives low to cut power: the panel's rails, the SD
// slot's, the battery divider's. Driven low is not enough on its own - in
// deep sleep a pad stops driving and floats, and a floating enable can half
// switch its load on for the whole night - so they are held low through the
// sleep, and released first thing on the next wake. The hold also keeps the
// EE02's panel switch, on the UART's TX pad, from following the boot ROM's
// log output high as the chip wakes.
constexpr int kSleepLow[] = {pins::kPower, pins::kSdEnable, pins::kBatteryEnable};

void holdLowForSleep() {
  for (int pin : kSleepLow) {
    if (pin < 0) continue;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(gpio_num_t(pin));
  }
  gpio_deep_sleep_hold_en();
}

void releaseSleepHolds() {
  for (int pin : kSleepLow)
    if (pin >= 0) gpio_hold_dis(gpio_num_t(pin));
}

void goToSleep() {
  ui.stop();
  WiFi.mode(WIFI_OFF);
  saveState(app.state);
  app.unmountCard();  // a powered card idles at a milliamp or two; the slot goes dark with the chip

  // The interval runs wake to wake, so the time spent awake comes off the
  // sleep; a wake that outlasts the interval goes straight into the next.
  const std::time_t now = std::time(nullptr);
  uint64_t seconds = app.sleepSeconds(now);
  if (!app.inQuietHours(now)) {
    const uint64_t awake = millis() / 1000;
    seconds = seconds > awake ? seconds - awake : 1;
  }
  if (const int mv = app.batteryMv(); mv >= 0) timing::set(timing::BatteryMv, mv);
  timing::finish(seconds);
  Serial.println(timing::summary().c_str());
  Serial.printf("sleep: %llu s\n", (unsigned long long)seconds);
  Serial.flush();

  // The keys wake the frame. They are pulled up in the RTC domain so a press
  // reads low while the rest of the chip is off.
  const uint64_t mask = (1ULL << pins::kKey1) | (1ULL << pins::kKey2) | (1ULL << pins::kKey3);
  for (int pin : {pins::kKey1, pins::kKey2, pins::kKey3}) {
    rtc_gpio_pullup_en(gpio_num_t(pin));
    rtc_gpio_pulldown_dis(gpio_num_t(pin));
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  holdLowForSleep();
  esp_deep_sleep_start();
}

// The ordinary cycle, once WiFi is up: fetch, choose, draw. Records what
// happened in State either way. Every page is a new layout, so the same
// birds come out in new places each time. With `skipIfSame`, a page that
// would come out exactly as the one already on the glass is not drawn at all -
// which, with the layout always moving on, is now never.
void drawNewPage(bool skipIfSame = false) {
  ++app.state.layout;
  app.progress("syncing the clock");
  syncClock();
  std::vector<int> page;
  const bool got = app.fetchBirds(page);
  app.state.fetchOk = got;
  if (!got) {
    app.state.lastResult = app.fetchError;
    Serial.printf("fetch: %s\n", app.fetchError.c_str());
    // Nothing on the glass yet is the one case a failure should show; a page
    // that is already up is better than an error over it.
    if (app.state.lastRender == 0 || app.state.showingStatus) app.showStatus();
    else timing::outcome("no_fetch");
    app.progress("");
    return;
  }
  // While the radio is still up: the weather, when a line of the page asks.
  app.fetchWeather();
  if (!app.weatherError.empty()) Serial.printf("weather: %s\n", app.weatherError.c_str());
  const uint32_t sig = app.pageSignature();
  if (skipIfSame && app.state.glass == "birds" && sig == app.state.pageSig) {
    app.state.lastResult = "unchanged - kept the page on the glass";
    Serial.println("page: unchanged, not redrawn");
    timing::outcome("unchanged");
    app.progress("");
    return;
  }
  std::string names;
  for (size_t i = 0; i < app.pageBirds.size(); ++i)
    names += (i ? ", " : "") + (app.pageCommon[i].empty() ? app.pageBirds[i] : app.pageCommon[i]);
  Serial.printf("page: %s\n", names.c_str());
  if (app.showBirds(page)) {
    app.recordShown(app.pageBirds);
    app.state.lastRender = uint32_t(std::time(nullptr));
    app.state.lastBirds = names;
    app.state.lastResult = "drew " + std::to_string(page.size()) + " birds";
    app.state.pageSig = sig;
    app.state.showingStatus = false;
  } else {
    app.state.lastResult = app.renderOutOfMemory ? "render ran out of memory" : "render failed";
    timing::outcome(app.renderOutOfMemory ? "oom" : "failed");
  }
  app.progress("");
}

// Serve the web UI until told to stop, keeping the keys alive meanwhile.
// `captive` is the access-point case. `untilSetUp` keeps it open with no
// idle timeout until the frame has a network and a source it can ask -
// there is nothing else for it to do, and sleeping would only hide it -
// and returns as soon as it does. Returns when the portal should close.
void servePortal(bool captive, bool untilSetUp = false) {
  // Wall time here, less what it set off that is timed on its own - a page
  // drawn from the web UI is render and panel time, not portal time.
  const uint32_t portalAt = millis(), accountedAt = timing::accounted();
  struct Filed {
    uint32_t at, before;
    ~Filed() {
      const uint32_t spent = millis() - at, inner = timing::accounted() - before;
      timing::add(timing::Portal, spent > inner ? spent - inner : 0);
    }
  } filed{portalAt, accountedAt};
  ui.begin(captive);
  app.portalForSetup = untilSetUp;
  uint32_t lastTouch = 0;
  const auto touch = [&] {
    lastTouch = millis();
    app.portalSleepAt = untilSetUp ? 0 : std::time(nullptr) + kPortalIdleMs / 1000;
  };
  touch();
  for (;;) {
    if (ui.poll()) touch();
    if (untilSetUp && app.settings.configured() && app.settings.sourceConfigured() &&
        WiFi.status() == WL_CONNECTED) {
      // Set up while we waited: on with the ordinary cycle.
      app.portalForSetup = false;
      return;
    }
    switch (ui.take()) {
      case WebUi::Request::Reboot:
        delay(500);
        ESP.restart();
        break;
      case WebUi::Request::Sleep:
        delay(300);
        app.state.portalOn = false;
        app.portalSleepAt = 0;
        return;
      case WebUi::Request::Refresh:
        if (WiFi.status() == WL_CONNECTED) {
          drawNewPage();
        } else {
          app.fetchError = "not joined to a network";
        }
        touch();
        break;
      case WebUi::Request::Stress:
#ifdef BIRDPOSTER_DEBUG
        if (WiFi.status() == WL_CONNECTED) app.stressTest(ui.stressPages);
#endif
        touch();
        break;
      case WebUi::Request::None:
        break;
    }
    switch (keyPressed()) {
      case Key::One:
        touch();  // on only: off is the web UI's "Done" or the idle timeout
        break;
      case Key::Two:
        app.state.showingStatus = !app.state.showingStatus;
        if (app.state.showingStatus) app.showStatus();
        else if (WiFi.status() == WL_CONNECTED) drawNewPage();
        touch();
        break;
      case Key::Three:
        if (WiFi.status() == WL_CONNECTED) {
          drawNewPage();
        }
        touch();
        break;
      case Key::None:
        break;
    }
    if (!untilSetUp && millis() - lastTouch > kPortalIdleMs) {
      app.state.portalOn = false;
      app.portalSleepAt = 0;
      return;
    }
    delay(5);
  }
}

// No network to join, or none that answers: the frame's own, with the page
// that says how to reach it.
void setupMode(const std::string &why) {
  startAp();
  // Only redraw if the glass shows something else: a frame that cannot reach
  // its router wakes into this every hour, and the page does not change.
  if (app.state.glass != "setup")
    app.showSetup(app.apSsid(), app.settings.apPass, kApUrl,
                  app.settings.configured() ? why : std::string());
  Serial.printf("setup mode: %s\n", why.c_str());
  // No network at all: nothing to do but wait to be told one, however long.
  servePortal(true, !app.settings.configured());
}

}  // namespace

// The hot loops' working sets in internal SRAM (fastmem.h), as long as the
// WiFi stack and a TLS session keep the room they need; past that, PSRAM.
static constexpr size_t kInternalReserve = 64 * 1024;

static void *internalAlloc(size_t bytes) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes ||
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes + kInternalReserve)
    return nullptr;
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void setup() {
  // Before anything drives the power switches again: see holdLowForSleep.
  releaseSleepHolds();
  setFastAlloc(internalAlloc);
  startCoreHelper();
  Serial.begin(115200);
  timing::start(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER  ? "timer"
                : esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 ? "key"
                                                                        : "power");
  pinMode(pins::kKey1, INPUT_PULLUP);
  pinMode(pins::kKey2, INPUT_PULLUP);
  pinMode(pins::kKey3, INPUT_PULLUP);
  const Key woke = keyFromWake();
  {
    timing::Scope t(timing::Settle);
    delay(woke == Key::None && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED ? 1500
                                                                                          : 100);
  }
  Serial.printf("\nbird poster %s, woke by %s\n", kFirmwareVersion,
                woke == Key::None ? (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER
                                         ? "timer"
                                         : "power")
                                  : "a key");
  if (!psramFound()) Serial.println("WARNING: no PSRAM - this build cannot render a page");

  const uint32_t beginAt = millis();
  const bool begun = app.begin();
  timing::add(timing::Begin, millis() - beginAt);
  if (!begun) {
    Serial.printf("app: %s%s\n", app.platesError.c_str(), app.fontOk ? "" : " (and no font)");
  }
  // Keep answering the settings page through the long blocking stages.
  app.onProgress = [] { ui.poll(); };
  // A key held during a cold boot counts as a press, for the bench.
  Key key = woke;
  if (key == Key::None) key = keyPressed();

  if (key == Key::One) app.state.portalOn = true;
  if (key == Key::Two) app.state.showingStatus = !app.state.showingStatus;
  // Key 3 is "new page now": a bird page, whatever the mode. Status mode is
  // left for key 2 to put back, rather than swallowing the press.
  if (key == Key::Three) app.state.showingStatus = false;
  saveState(app.state);

  if (!app.settings.configured()) {
    setupMode("no network configured");
    goToSleep();
  }

  const bool joined = joinWifi();
  if (!joined) {
    ++app.state.wifiFailures;
    app.state.fetchOk = false;
    app.state.lastResult = "could not join " + app.settings.wifiSsid +
                           (wifiReason ? " - " + wifiReasonText() : std::string());
    if (app.state.portalOn || app.state.wifiFailures >= kFailuresBeforeAp) {
      app.state.portalOn = false;
      setupMode(app.state.lastResult);
    } else if (app.state.showingStatus) {
      app.showStatus();
    }
    goToSleep();
  }
  app.state.wifiFailures = 0;
  // A failed join is the last result until something else happens, and a
  // wake that only serves the settings fetches nothing to replace it - so the
  // page would go on saying it could not join while being served over it.
  if (app.state.lastResult.rfind("could not join", 0) == 0)
    app.state.lastResult = "joined " + app.settings.wifiSsid + "; no fetch since";

  if (app.state.portalOn && app.settings.sourceConfigured()) {
    syncClock();
    // Straight from the setup network, the glass still gives that network's
    // address, which has just gone. Show the one the frame has now.
    if (app.state.glass == "setup") app.showStatus();
    Serial.println("portal: on");
    servePortal(false);
    goToSleep();
  }

  if (!app.settings.sourceConfigured()) {
    // Joined, but nothing to ask yet: show where the settings are and stay
    // reachable until they are filled in, then carry straight on to a page.
    syncClock();
    app.state.showingStatus = true;
    app.showStatus();
    Serial.println("setup: joined, waiting for a source");
    servePortal(false, true);
    app.state.showingStatus = false;
    // Still serving the settings after this page if WiFi was switched on.
    if (!app.state.portalOn) app.beforeRefresh = app.beforeRender = radioOff;
    drawNewPage();
    // Just joined from the setup network: stay reachable a while after the
    // first page, since whoever set it up is likely still at the settings.
    if (app.state.portalOn) servePortal(false);
    goToSleep();
  }

  if (app.state.showingStatus) {
    // Try the source so the page can say whether it answers, then show it.
    syncClock();
    std::vector<int> page;
    app.state.fetchOk = app.fetchBirds(page);
    if (!app.state.fetchOk) app.state.lastResult = app.fetchError;
    app.fetchWeather();  // so the status page can say whether it answers
    app.beforeRefresh = radioOff;
    app.showStatus();
    goToSleep();
  }

  if (woke != Key::None || !app.inQuietHours()) {
    // Off as soon as the page's layout and web plates are in, not just for
    // the refresh: the render runs twenty-odd seconds without the network.
    app.beforeRefresh = app.beforeRender = radioOff;
    drawNewPage(woke == Key::None);  // a key asked for a page: draw it regardless
  }
  else {
    Serial.println("quiet hours: not drawing");
    timing::outcome("quiet");
  }
  goToSleep();
}

void loop() { delay(1000); }
