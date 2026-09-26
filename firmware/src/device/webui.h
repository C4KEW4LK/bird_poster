// The settings page the frame serves, and the captive portal that puts a
// phone straight onto it.
//
// One page: status and a preview of the glass at the top, the settings below,
// actions at the bottom. Rendered from a template in flash with the current
// values substituted, so it works with scripts off and needs nothing fetched.
//
// In access-point mode a DNS server answers every name with the frame's own
// address and the OS connectivity probes (Android's generate_204, Apple's
// hotspot-detect, Windows' ncsi) are redirected to the page, which is what
// makes the phone's "sign in to network" prompt open the settings by itself.
#pragma once

#include "app.h"

namespace birdposter {

class WebUi {
 public:
  explicit WebUi(App &app) : app_(app) {}

  // Start serving. `captive` adds the DNS catch-all for access-point mode.
  void begin(bool captive);
  void stop();
  // Call from loop(); returns true if a request was handled since the last
  // call, which the portal uses as its idle timer.
  bool poll();

  // Set by actions that the request handler cannot finish itself: a reboot,
  // or leaving the portal. main.cpp reads and acts on them.
  enum class Request { None, Reboot, Sleep, Refresh };
  Request take();

 private:
  App &app_;
  Request pending_ = Request::None;
  bool captive_ = false;
  bool active_ = false;
  bool inHandler_ = false;  // a handler that reports progress must not re-enter the server
  std::string warning_;     // shown once on the next page: what a save could not take
};

}  // namespace birdposter
