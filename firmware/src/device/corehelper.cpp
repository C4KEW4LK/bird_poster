#include "corehelper.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace birdposter {

namespace {

class CoreHelper : public Helper {
 public:
  bool begin() {
    go_ = xSemaphoreCreateBinary();
    done_ = xSemaphoreCreateBinary();
    if (!go_ || !done_) return false;
    // 4 KB of stack: the job is a dither row's preparing, a few locals deep.
    return xTaskCreatePinnedToCore(&CoreHelper::loop, "render-helper", 4096, this, 1, &task_, 0) ==
           pdPASS;
  }
  void start(void (*fn)(void *), void *arg) override {
    fn_ = fn;
    arg_ = arg;
    xSemaphoreGive(go_);
  }
  void wait() override { xSemaphoreTake(done_, portMAX_DELAY); }

 private:
  static void loop(void *self) {
    auto *h = static_cast<CoreHelper *>(self);
    for (;;) {
      xSemaphoreTake(h->go_, portMAX_DELAY);
      h->fn_(h->arg_);
      xSemaphoreGive(h->done_);
    }
  }
  SemaphoreHandle_t go_ = nullptr, done_ = nullptr;
  TaskHandle_t task_ = nullptr;
  void (*fn_)(void *) = nullptr;
  void *arg_ = nullptr;
};

CoreHelper helperCore;

}  // namespace

void startCoreHelper() {
  if (helperCore.begin())
    setHelper(&helperCore);
  else
    Serial.println("core helper: could not start; the render stays on one core");
}

}  // namespace birdposter
