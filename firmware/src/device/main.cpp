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

bool joinWifi() {
  if (!app.settings.configured()) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(app.settings.hostname.c_str());
  WiFi.begin(app.settings.wifiSsid.c_str(), app.settings.wifiPass.c_str());
  Serial.printf("wifi: joining %s", app.settings.wifiSsid.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kJoinTimeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi: failed");
    return false;
  }
  Serial.printf("wifi: %s\n", WiFi.localIP().toString().c_str());
  MDNS.begin(app.settings.hostname.c_str());
  MDNS.addService("http", "tcp", 80);
  return true;
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
// happened in State either way.
void drawNewPage() {
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
  std::string names;
  for (size_t i = 0; i < app.pageBirds.size(); ++i)
    names += (i ? ", " : "") + (app.pageCommon[i].empty() ? app.pageBirds[i] : app.pageCommon[i]);
  Serial.printf("page: %s\n", names.c_str());
  if (app.showBirds(page)) {
    app.recordShown(app.pageBirds);
    app.state.lastRender = uint32_t(std::time(nullptr));
    app.state.lastBirds = names;
    app.state.lastResult = "drew " + std::to_string(page.size()) + " birds";
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
    app.showStatus();
    goToSleep();
  }

  if (woke != Key::None || !app.inQuietHours()) drawNewPage();
  else Serial.println("quiet hours: not drawing");
  goToSleep();
}

void loop() { delay(1000); }
