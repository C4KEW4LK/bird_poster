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
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "app.h"
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

bool joinWifi() {
  if (!app.settings.configured()) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(app.settings.hostname.c_str());
  const char *ssid = app.settings.wifiSsid.c_str(), *pass = app.settings.wifiPass.c_str();
  const uint32_t start = millis();
  const uint32_t key = networkKey();
  bool joined = false;
  if (rtcChannel > 0 && rtcKey == key) {
    Serial.printf("wifi: joining %s on channel %d\n", ssid, int(rtcChannel));
    WiFi.begin(ssid, pass, rtcChannel, rtcBssid);
    joined = waitJoined(kFastJoinMs);
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
    WiFi.begin(ssid, pass);
    // What is left of the usual budget, but never under five seconds for the scan.
    const uint32_t spent = uint32_t(millis() - start);
    joined = waitJoined(kJoinTimeoutMs - std::min<uint32_t>(kJoinTimeoutMs - 5000, spent));
  }
  if (!joined) {
    Serial.println("wifi: failed");
    return false;
  }
  memcpy(rtcBssid, WiFi.BSSID(), sizeof rtcBssid);
  rtcChannel = WiFi.channel();
  rtcKey = key;
  Serial.printf("wifi: %s in %lu ms\n", WiFi.localIP().toString().c_str(),
                (unsigned long)(millis() - start));
  MDNS.begin(app.settings.hostname.c_str());
  MDNS.addService("http", "tcp", 80);
  return true;
}

// The radio off for good this wake. Called just before the glass refresh
// when the frame will sleep straight after it: nothing past that point
// needs the network, and the refresh is the longest thing a wake does.
void radioOff() {
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("wifi: off for the refresh");
}

void syncClock() {
  configTzTime(app.settings.tz.c_str(), "pool.ntp.org", "time.google.com");
  const uint32_t start = millis();
  while (std::time(nullptr) < 100000 && millis() - start < kNtpTimeoutMs) delay(100);
  Serial.printf("clock: %s\n", app.localTime(std::time(nullptr)).c_str());
}

void startAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);  // the station side is what a network scan needs
  WiFi.softAP(app.apSsid().c_str(), app.settings.apPass.c_str());
  delay(100);
  Serial.printf("ap: %s at %s\n", app.apSsid().c_str(), WiFi.softAPIP().toString().c_str());
}

void goToSleep() {
  ui.stop();
  WiFi.mode(WIFI_OFF);
  saveState(app.state);
  app.unmountCard();  // a powered card idles at a milliamp or two; the slot goes dark with the chip

  const uint64_t seconds = app.sleepSeconds(std::time(nullptr));
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
  esp_deep_sleep_start();
}

// The ordinary cycle, once WiFi is up: fetch, choose, draw. Records what
// happened in State either way. With `skipIfSame`, a page that would come out
// exactly as the one already on the glass is not drawn at all: the refresh is
// the most expensive thing a wake does, and redrawing the same birds in the
// same places buys nothing.
void drawNewPage(bool skipIfSame = false) {
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
    app.progress("");
    return;
  }
  const uint32_t sig = app.pageSignature();
  if (skipIfSame && app.state.glass == "birds" && sig == app.state.pageSig) {
    app.state.lastResult = "unchanged - kept the page on the glass";
    Serial.println("page: unchanged, not redrawn");
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
    app.state.lastResult = "render failed";
  }
  app.progress("");
}

// Serve the web UI until told to stop, keeping the keys alive meanwhile.
// `captive` is the access-point case. `untilSetUp` keeps it open with no
// idle timeout until the frame has a network and a source it can ask -
// there is nothing else for it to do, and sleeping would only hide it -
// and returns as soon as it does. Returns when the portal should close.
void servePortal(bool captive, bool untilSetUp = false) {
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
          ++app.state.layout;
          drawNewPage();
        } else {
          app.fetchError = "not joined to a network";
        }
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
          ++app.state.layout;
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

void setup() {
  Serial.begin(115200);
  pinMode(pins::kKey1, INPUT_PULLUP);
  pinMode(pins::kKey2, INPUT_PULLUP);
  pinMode(pins::kKey3, INPUT_PULLUP);
  const Key woke = keyFromWake();
  delay(woke == Key::None && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED ? 1500
                                                                                        : 100);
  Serial.printf("\nbird poster %s, woke by %s\n", kFirmwareVersion,
                woke == Key::None ? (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER
                                         ? "timer"
                                         : "power")
                                  : "a key");
  if (!psramFound()) Serial.println("WARNING: no PSRAM - this build cannot render a page");

  if (!app.begin()) {
    Serial.printf("app: %s%s\n", app.platesError.c_str(), app.fontOk ? "" : " (and no font)");
  }
  // Keep answering the settings page through the long blocking stages.
  app.onProgress = [] { ui.poll(); };
  // A key held during a cold boot counts as a press, for the bench.
  Key key = woke;
  if (key == Key::None) key = keyPressed();

  if (key == Key::One) app.state.portalOn = true;
  if (key == Key::Two) app.state.showingStatus = !app.state.showingStatus;
  if (key == Key::Three) ++app.state.layout;
  saveState(app.state);

  if (!app.settings.configured()) {
    setupMode("no network configured");
    goToSleep();
  }

  const bool joined = joinWifi();
  if (!joined) {
    ++app.state.wifiFailures;
    app.state.fetchOk = false;
    app.state.lastResult = "could not join " + app.settings.wifiSsid;
    if (app.state.portalOn || app.state.wifiFailures >= kFailuresBeforeAp) {
      app.state.portalOn = false;
      setupMode(app.state.lastResult);
    } else if (app.state.showingStatus) {
      app.showStatus();
    }
    goToSleep();
  }
  app.state.wifiFailures = 0;

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
    ++app.state.layout;
    // Still serving the settings after this page if WiFi was switched on.
    if (!app.state.portalOn) app.beforeRefresh = radioOff;
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
    app.beforeRefresh = radioOff;
    app.showStatus();
    goToSleep();
  }

  if (woke != Key::None || !app.inQuietHours()) {
    app.beforeRefresh = radioOff;
    drawNewPage(woke == Key::None);  // a key asked for a page: draw it regardless
  }
  else Serial.println("quiet hours: not drawing");
  goToSleep();
}

void loop() { delay(1000); }
