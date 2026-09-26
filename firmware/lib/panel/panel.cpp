#include "panel.h"

#include <Arduino.h>
#include <SPI.h>

namespace birdposter {

namespace {

// Seeed_GFX2 drives this panel at 10 MHz; the ESPHome port found 2 MHz the
// safe ceiling on its wiring. 10 MHz streams the 960 KB frame in under a
// second and is what the vendor ships, so start there.
constexpr uint32_t kSpiHz = 10000000;
constexpr uint32_t kBusyTimeoutMs = 90000;  // a full six-colour refresh is ~30 s

// The panel's own codes. Index by Ink: black, white, yellow, red, blue, green.
// 4 is unused on this glass, which is why blue and green sit at 5 and 6.
constexpr uint8_t kCode[6] = {0x0, 0x1, 0x2, 0x3, 0x5, 0x6};

SPIClass spi(FSPI);

void idleSelects() {
  pinMode(pins::kCsMaster, OUTPUT);
  pinMode(pins::kCsSlave, OUTPUT);
  digitalWrite(pins::kCsMaster, HIGH);
  digitalWrite(pins::kCsSlave, HIGH);
}

}  // namespace

void Panel::select(Chip chip) {
  if (chip != Chip::Slave) digitalWrite(pins::kCsMaster, LOW);
  if (chip != Chip::Master) digitalWrite(pins::kCsSlave, LOW);
}

void Panel::deselect() {
  digitalWrite(pins::kCsMaster, HIGH);
  digitalWrite(pins::kCsSlave, HIGH);
}

void Panel::command(Chip chip, uint8_t cmd, const uint8_t *data, size_t len) {
  spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  select(chip);
  digitalWrite(pins::kDc, LOW);
  spi.transfer(cmd);
  if (data && len) {
    digitalWrite(pins::kDc, HIGH);
    spi.writeBytes(data, len);
  }
  deselect();
  spi.endTransaction();
}

bool Panel::waitReady(uint32_t timeoutMs) {
  // The controller takes a moment to pull BUSY low after a command, so a
  // stale high would read as "done" if sampled at once.
  delay(10);
  const uint32_t start = millis();
  while (digitalRead(pins::kBusy) == LOW) {
    if (millis() - start > timeoutMs) return false;
    if (onWait) onWait();
    delay(10);
  }
  return true;
}

::SPIClass &sharedSpi() {
  // SPIClass::begin is a no-op once the bus is up, so every caller may ask.
  idleSelects();
  spi.begin(pins::kSck, pins::kSdMiso, pins::kMosi, -1);
  return spi;
}

bool Panel::begin() {
  idleSelects();
  pinMode(pins::kDc, OUTPUT);
  pinMode(pins::kReset, OUTPUT);
  pinMode(pins::kPower, OUTPUT);
  pinMode(pins::kBusy, INPUT);
  deselect();
  digitalWrite(pins::kDc, HIGH);
  digitalWrite(pins::kPower, HIGH);
  delay(20);

  sharedSpi();

  digitalWrite(pins::kReset, LOW);
  delay(20);
  digitalWrite(pins::kReset, HIGH);
  delay(20);
  if (!waitReady(5000)) {
    ok_ = false;
    return false;
  }

  // Seeed_GFX2 Driver_T133A01::init, byte for byte.
  static const uint8_t r74[] = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
  static const uint8_t rf0[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
  static const uint8_t psr[] = {0xDF, 0x69};
  static const uint8_t dcdc[] = {0x44, 0x54, 0x00};
  static const uint8_t cdi[] = {0x37};
  static const uint8_t r60[] = {0x03, 0x03};
  static const uint8_t r86[] = {0x10};
  static const uint8_t pws[] = {0x22};
  static const uint8_t tres[] = {0x04, 0xB0, 0x03, 0x20};  // 1200 x 800 per controller
  static const uint8_t pwr[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
  static const uint8_t rb6[] = {0x07};
  static const uint8_t btst[] = {0xE0, 0x20};
  static const uint8_t rb7[] = {0x01};
  static const uint8_t rb0[] = {0x01};
  static const uint8_t rb1[] = {0x02};

  command(Chip::Master, 0x74, r74, sizeof r74);
  command(Chip::Both, 0xF0, rf0, sizeof rf0);
  delay(10);
  command(Chip::Both, 0x00, psr, sizeof psr);
  delay(10);
  command(Chip::Master, 0xA5, dcdc, sizeof dcdc);
  delay(10);
  command(Chip::Both, 0x50, cdi, sizeof cdi);
  delay(10);
  command(Chip::Both, 0x60, r60, sizeof r60);
  delay(10);
  command(Chip::Both, 0x86, r86, sizeof r86);
  delay(10);
  command(Chip::Both, 0xE3, pws, sizeof pws);
  delay(10);
  command(Chip::Both, 0x61, tres, sizeof tres);
  delay(10);
  command(Chip::Master, 0x01, pwr, sizeof pwr);
  delay(10);
  command(Chip::Master, 0xB6, rb6, sizeof rb6);
  delay(10);
  command(Chip::Master, 0x06, btst, sizeof btst);
  delay(10);
  command(Chip::Master, 0xB7, rb7, sizeof rb7);
  delay(10);
  command(Chip::Master, 0x05, btst, sizeof btst);
  delay(10);
  command(Chip::Master, 0xB0, rb0, sizeof rb0);
  delay(10);
  command(Chip::Master, 0xB1, rb1, sizeof rb1);
  delay(10);
  ok_ = true;
  return true;
}

bool Panel::push(const Frame &frame, int rotation) {
  rotation &= 3;
  const bool turned = rotation & 1;
  const int wantW = turned ? kPanelH : kPanelW, wantH = turned ? kPanelW : kPanelH;
  if (frame.w != wantW || frame.h != wantH) return false;

  static const uint8_t ccset = 0x01;
  command(Chip::Both, 0xE0, &ccset, 1);
  if (!waitReady(5000)) return false;

  // Each controller takes its 600 columns of every row, two pixels a byte,
  // left pixel in the high nibble. The turn is applied while packing, so the
  // frame is read once and nothing is copied.
  constexpr int kHalf = kPanelW / 2;
  uint8_t row[kHalf / 2];
  for (int chip = 0; chip < 2; ++chip) {
    spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
    select(chip == 0 ? Chip::Master : Chip::Slave);
    digitalWrite(pins::kDc, LOW);
    spi.transfer(0x10);
    digitalWrite(pins::kDc, HIGH);
    for (int ny = 0; ny < kPanelH; ++ny) {
      for (int i = 0; i < kHalf; i += 2) {
        uint8_t packed = 0;
        for (int k = 0; k < 2; ++k) {
          const int nx = chip * kHalf + i + k;
          int lx, ly;
          switch (rotation) {
            case 0: lx = nx; ly = ny; break;
            case 1: lx = ny; ly = kPanelW - 1 - nx; break;
            case 2: lx = kPanelW - 1 - nx; ly = kPanelH - 1 - ny; break;
            default: lx = kPanelH - 1 - ny; ly = nx; break;
          }
          packed = uint8_t((packed << 4) | kCode[frame.row(ly)[lx] % 6]);
        }
        row[i / 2] = packed;
      }
      spi.writeBytes(row, sizeof row);
      if ((ny & 63) == 0) yield();
    }
    deselect();
    spi.endTransaction();
  }
  return true;
}

bool Panel::refresh() {
  static const uint8_t drf = 0x01;
  static const uint8_t pof = 0x00;
  command(Chip::Both, 0x04);  // power on
  if (!waitReady(kBusyTimeoutMs)) return false;
  delay(30);
  command(Chip::Both, 0x12, &drf, 1);  // display refresh
  if (!waitReady(kBusyTimeoutMs)) return false;
  delay(30);
  command(Chip::Both, 0x02, &pof, 1);  // power off
  const bool ok = waitReady(kBusyTimeoutMs);
  delay(30);
  return ok;
}

void Panel::sleep() {
  static const uint8_t deep = 0xA5;
  command(Chip::Master, 0x07, &deep, 1);
  waitReady(2000);
  digitalWrite(pins::kPower, LOW);
  ok_ = false;
}

}  // namespace birdposter
