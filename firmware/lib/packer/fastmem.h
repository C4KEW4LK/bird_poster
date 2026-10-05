// Memory for the hot loops' working sets.
//
// On the frame, anything over 4 KB is allocated from PSRAM, which sits behind
// a 32 KB cache with 32-byte lines. A buffer that a loop sweeps once is fine
// there; a working set it comes back to on every pixel - the dither's rows of
// error and colour, the plane coder's context models - is larger than the
// cache when taken together, and every pass misses. Those few buffers are
// allocated through here instead, which the device points at internal SRAM.
//
// Internal SRAM is short (the WiFi stack wants most of it), so the hook may
// refuse, and the allocation then falls back to the ordinary heap: slower,
// never wrong. On the host the hook is unset and this is plain malloc.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace birdposter {

// Returns memory for `bytes`, or null to fall back to malloc. Whatever it
// returns must be releasable with free(), as ESP-IDF's heap_caps_malloc is.
using FastAllocHook = void *(*)(size_t bytes);
void setFastAlloc(FastAllocHook hook);
void *fastAlloc(size_t bytes);

// What fastAlloc has handed out since it was last cleared: bytes the hook
// gave (fast memory) and bytes it fell back to malloc for. A hot buffer that
// quietly landed in PSRAM shows here and nowhere else.
struct FastStats {
  size_t fastBytes = 0, fallbackBytes = 0;
  int fallbacks = 0;
};
extern FastStats fastStats;

template <class T>
struct FastAllocator {
  using value_type = T;
  FastAllocator() = default;
  template <class U>
  FastAllocator(const FastAllocator<U> &) {}
  T *allocate(size_t n) {
    void *p = fastAlloc(n * sizeof(T));
    if (!p) throw std::bad_alloc();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) { std::free(p); }
  template <class U>
  bool operator==(const FastAllocator<U> &) const { return true; }
  template <class U>
  bool operator!=(const FastAllocator<U> &) const { return false; }
};

template <class T>
using FastVector = std::vector<T, FastAllocator<T>>;

}  // namespace birdposter
