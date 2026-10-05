// The S3's other core, for the render (see Helper in render.h).
//
// Arduino runs setup() and loop() on core 1; core 0 carries WiFi and the
// system tasks and is otherwise idle. This puts one task there, at the lowest
// priority above idle, that runs what it is handed and signals when done -
// so WiFi and the system pre-empt it whenever they want the core, and between
// jobs it blocks, so the idle task (and its watchdog) always gets time.
#pragma once

#include "render.h"

namespace birdposter {

// Start the task on core 0 and make it the render's Helper. Call once.
void startCoreHelper();

}  // namespace birdposter
