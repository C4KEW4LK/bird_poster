// A few seconds of measurements of the chip itself, served as /api/bench.
//
// The render's costs were first guessed by comparing the frame against the
// desktop harness, and the guesses were off by orders of magnitude in both
// directions. This measures the things the guesses hinged on, on the frame:
// how fast internal RAM and PSRAM are sequentially and at random (in and out
// of the 32 KB cache), what the 32-bit core pays for 64-bit shifts, divides
// and doubles, and how fast the plate pack reads off flash. Run on request
// only - it is a few seconds at full clock - and never on an ordinary wake.
// See notes/esp32-port/performance.md for what the numbers have meant so far.
#pragma once

#include <string>

namespace birdposter::bench {

// Every measurement as JSON. `packPath` is the plate pack on LittleFS, read
// for the flash figures; empty skips them.
std::string run(const std::string &packPath);

}  // namespace birdposter::bench
