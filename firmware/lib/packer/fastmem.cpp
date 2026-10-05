#include "fastmem.h"

namespace birdposter {

namespace {
FastAllocHook fastHook = nullptr;
}  // namespace

FastStats fastStats;

void setFastAlloc(FastAllocHook hook) { fastHook = hook; }

void *fastAlloc(size_t bytes) {
  if (bytes == 0) bytes = 1;
  if (fastHook)
    if (void *p = fastHook(bytes)) {
      fastStats.fastBytes += bytes;
      return p;
    }
  fastStats.fallbackBytes += bytes;
  ++fastStats.fallbacks;
  return std::malloc(bytes);
}

}  // namespace birdposter
