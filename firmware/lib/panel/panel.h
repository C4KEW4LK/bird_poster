// The 13.3" Spectra 6 panel on the EE02 or the reTerminal E1004, driven directly.
//
// The glass is a T133A01: 1200 x 1600 native, four bits a pixel, and two
// controllers side by side, each owning 600 columns and wanting its own chip
// select. The register sequence is Seeed's own (Seeed_GFX2, Driver_T133A01),
// which is the vendor's hardware-verified init for this exact board; the
// framing and the BUSY polarity agree with the independent ESPHome port. Both
// are cited in notes/esp32-port/IMPLEMENTATION.md §1, which also has the pins.
//
// This is all the frame needs from a display library - init, stream a frame,
// refresh, sleep - so it is written out rather than carrying Seeed_GFX2's
// drawing API, product catalog and a second framebuffer along. The colour
// codes are the panel's, and they are what lib/render already produces.
#pragma once

#include <cstdint>
#include <functional>

#include "render.h"

class SPIClass;  // Arduino's; the header itself stays out of here

namespace birdposter {

// Physical GPIO numbers. The same glass and the same two controllers sit
// behind two different boards:
//
//   EE02   Seeed's driver board for a bare XIAO ESP32-S3 Plus. GPIO43 is
//          UART0 TX on the XIAO and the panel's power enable here, which is
//          why Serial has to be the USB CDC port.
//   E1004  Seeed's reTerminal E1004: the same panel, an ESP32-S3 with 32 MB
//          of flash, a battery, an SD slot and a case. Pins from Seeed's own
//          GxEPD2_reTerminal_E1004 example and the peripherals cookbook.
//
// Both sets of keys are on RTC-capable pins, so all three wake the frame.
#if defined(BOARD_E1004)
namespace pins {
constexpr int kSck = 7;
constexpr int kMosi = 9;
constexpr int kCsMaster = 10;
constexpr int kCsSlave = 2;
constexpr int kDc = 11;
constexpr int kBusy = 13;  // LOW while busy
constexpr int kReset = 38;
constexpr int kPower = 12; // HIGH = panel powered
constexpr int kKey1 = 3;   // KEY0-KEY2, the front-panel buttons, active low
constexpr int kKey2 = 4;
constexpr int kKey3 = 5;
constexpr int kBatteryAdc = 1;     // through a 1:2 divider, read with kBatteryEnable high
constexpr int kBatteryEnable = 21;
constexpr int kLed = 48;
// The microSD slot. It shares SCK and MOSI with the panel - one bus, two
// chip selects - and brings the only MISO on it, since the panel is write-only.
// Pins from Seeed's MicRecordToSD sketch in the peripherals cookbook.
constexpr int kSdCs = 14;
constexpr int kSdMiso = 8;
constexpr int kSdDetect = 15;  // LOW = card present
constexpr int kSdEnable = 16;  // HIGH = slot powered
}  // namespace pins
#else
namespace pins {
constexpr int kSck = 7;
constexpr int kMosi = 9;
constexpr int kCsMaster = 44;
constexpr int kCsSlave = 41;
constexpr int kDc = 10;
constexpr int kBusy = 4;   // LOW while busy
constexpr int kReset = 38;
constexpr int kPower = 43; // HIGH = panel powered
constexpr int kKey1 = 2;   // the three user keys, active low, all RTC-capable
constexpr int kKey2 = 3;
constexpr int kKey3 = 5;
constexpr int kBatteryAdc = -1;  // the EE02 brings no battery sense out
constexpr int kBatteryEnable = -1;
constexpr int kLed = -1;
constexpr int kSdCs = -1;        // no slot
constexpr int kSdMiso = -1;
constexpr int kSdDetect = -1;
constexpr int kSdEnable = -1;
}  // namespace pins
#endif

// The one SPI bus the panel and, on the E1004, the SD slot both hang off,
// begun on first use with whatever MISO the board has. Each side selects its
// own chip inside a transaction, so they take turns without knowing of each
// other; the panel's selects idle high from the moment anything touches the
// bus.
::SPIClass &sharedSpi();

constexpr int kPanelW = 1200;  // native, portrait
constexpr int kPanelH = 1600;

class Panel {
 public:
  // Power the panel, reset it and run the init sequence. False if BUSY never
  // released, which is what an unplugged FPC looks like.
  bool begin();

  // Stream a page to the controllers' RAM. `rotation` is quarter turns
  // clockwise from the native portrait: 0 and 2 take a 1200 x 1600 frame, 1
  // and 3 a 1600 x 1200 one. Nothing shows until refresh().
  bool push(const Frame &frame, int rotation);

  // Drive the update: about 30 s of flashing, blocking. Returns false on a
  // BUSY timeout.
  bool refresh();

  // Deep-sleep the controllers and cut the panel's power. The image stays.
  void sleep();

  // Whether begin() succeeded.
  bool ok() const { return ok_; }

  // Called every few ms while waiting on BUSY - a refresh is 30 s of it - so
  // the caller can keep serving its web UI meanwhile. May be empty.
  std::function<void()> onWait;

 private:
  enum class Chip { Master, Slave, Both };
  void select(Chip chip);
  void deselect();
  void command(Chip chip, uint8_t cmd, const uint8_t *data = nullptr, size_t len = 0);
  bool waitReady(uint32_t timeoutMs);
  bool ok_ = false;
};

}  // namespace birdposter
