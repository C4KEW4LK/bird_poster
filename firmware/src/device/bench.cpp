#include "bench.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

namespace birdposter::bench {

namespace {

// Results the compiler must not throw away.
volatile uint32_t sink32;
volatile uint64_t sink64;
volatile float sinkF;
volatile double sinkD;

int64_t now() { return esp_timer_get_time(); }

struct Json {
  std::string s = "{";
  bool first = true;
  void num(const char *k, double v, int places = 1) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s\"%s\":%.*f", first ? "" : ",", k, places, v);
    s += buf;
    first = false;
  }
  void raw(const char *k, const std::string &v) {
    s += std::string(first ? "" : ",") + "\"" + k + "\":" + v;
    first = false;
  }
  std::string done() { return s + "}"; }
};

// MB/s reading and writing `bytes` of `p` a 32-bit word at a time, best of three.
void sequential(Json &j, const char *name, uint32_t *p, size_t bytes) {
  const size_t n = bytes / 4;
  int64_t bestR = INT64_MAX, bestW = INT64_MAX;
  for (int rep = 0; rep < 3; ++rep) {
    int64_t t = now();
    for (size_t i = 0; i < n; ++i) p[i] = uint32_t(i);
    bestW = std::min(bestW, now() - t);
    t = now();
    uint32_t acc = 0;
    for (size_t i = 0; i < n; ++i) acc += p[i];
    sink32 = acc;
    bestR = std::min(bestR, now() - t);
  }
  Json r;
  r.num("read_mb_s", double(bytes) / double(bestR));
  r.num("write_mb_s", double(bytes) / double(bestW));
  j.raw(name, r.done());
}

// ns per 32-bit read at random places in `bytes` of `p`: cache hits when the
// span fits the cache, misses when it does not.
double randomRead(uint32_t *p, size_t bytes, int reads) {
  const uint32_t mask = uint32_t(bytes / 4 - 1);  // bytes is a power of two
  for (size_t i = 0; i <= mask; ++i) p[i] = uint32_t(i * 2654435761u);
  uint32_t x = 12345, acc = 0;
  const int64_t t = now();
  for (int i = 0; i < reads; ++i) {
    x = x * 1664525u + 1013904223u;
    acc += p[(x >> 7) & mask];
  }
  const int64_t us = now() - t;
  sink32 = acc;
  return double(us) * 1000.0 / reads;
}

// The same loop of random indices with no memory touched: what randomRead
// spends on itself, to take off its figures.
double randomBase(int reads) {
  uint32_t x = 12345, acc = 0;
  const int64_t t = now();
  for (int i = 0; i < reads; ++i) {
    x = x * 1664525u + 1013904223u;
    acc += (x >> 7);
  }
  const int64_t us = now() - t;
  sink32 = acc;
  return double(us) * 1000.0 / reads;
}

template <class F>
double nsPerOp(int n, F body) {
  const int64_t t = now();
  body(n);
  return double(now() - t) * 1000.0 / n;
}

std::string ops() {
  constexpr int n = 200000;
  Json j;
  const double base = nsPerOp(n, [](int k) {
    uint32_t a = 1;
    for (int i = 0; i < k; ++i) a += uint32_t(i);
    sink32 = a;
  });
  j.num("loop_ns", base, 2);
  // Each op is in a dependent chain, so it is the op's latency that counts.
  j.num("add32_ns", nsPerOp(n, [](int k) {
          uint32_t a = 1;
          for (int i = 0; i < k; ++i) a = a + uint32_t(i) + 1;
          sink32 = a;
        }), 2);
  j.num("shift32_var_ns", nsPerOp(n, [](int k) {
          uint32_t a = 0x12345678;
          for (int i = 0; i < k; ++i) a = (a >> (i & 31)) | (a << ((i + 7) & 31)) | 1u;
          sink32 = a;
        }), 2);
  j.num("shift64_var_ns", nsPerOp(n, [](int k) {
          uint64_t a = 0x123456789abcdefULL;
          for (int i = 0; i < k; ++i) a = (a >> (i & 63)) | (a << ((i + 7) & 63)) | 1u;
          sink64 = a;
        }), 2);
  j.num("mul32_ns", nsPerOp(n, [](int k) {
          uint32_t a = 3;
          for (int i = 0; i < k; ++i) a = a * 2654435761u + uint32_t(i);
          sink32 = a;
        }), 2);
  j.num("div32_ns", nsPerOp(n, [](int k) {
          uint32_t a = 0xFFFFFFFFu;
          for (int i = 0; i < k; ++i) a = (a / uint32_t((i & 15) + 3)) + 0xFFFF0000u;
          sink32 = a;
        }), 2);
  j.num("float_mul_ns", nsPerOp(n, [](int k) {
          float a = 1.0001f;
          for (int i = 0; i < k; ++i) a = a * 1.0000001f + 0.5f;
          sinkF = a;
        }), 2);
  j.num("float_div_ns", nsPerOp(n / 4, [](int k) {
          float a = 1e30f;
          for (int i = 0; i < k; ++i) a = a / 1.0000001f + 1.0f;
          sinkF = a;
        }), 2);
  j.num("double_mul_ns", nsPerOp(n / 4, [](int k) {
          double a = 1.0001;
          for (int i = 0; i < k; ++i) a = a * 1.0000001 + 0.5;
          sinkD = a;
        }), 2);
  j.num("double_div_ns", nsPerOp(n / 10, [](int k) {
          double a = 1e300;
          for (int i = 0; i < k; ++i) a = a / 1.0000001 + 1.0;
          sinkD = a;
        }), 2);
  j.num("cosf_ns", nsPerOp(n / 10, [](int k) {
          float a = 0.1f;
          for (int i = 0; i < k; ++i) a = cosf(a) + 0.001f;
          sinkF = a;
        }), 2);
  j.num("cos_double_ns", nsPerOp(n / 40, [](int k) {
          double a = 0.1;
          for (int i = 0; i < k; ++i) a = cos(a) + 0.001;
          sinkD = a;
        }), 2);
  return j.done();
}

std::string memory() {
  Json j;
  constexpr int kReads = 200000;
  const double base = randomBase(kReads);
  j.num("random_loop_ns", base, 2);

  // Internal: 32 KB, as much as can be had without crowding the WiFi stack.
  if (auto *p = static_cast<uint32_t *>(
          heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))) {
    sequential(j, "internal_seq", p, 32 * 1024);
    j.num("internal_random_ns", randomRead(p, 32 * 1024, kReads) - base, 2);
    heap_caps_free(p);
  }
  // PSRAM: a span that fits the 32 KB data cache, and one that cannot.
  // 2 MB is sixty times the cache, and free even with a page's frame held.
  if (auto *p = static_cast<uint32_t *>(heap_caps_malloc(2u << 20, MALLOC_CAP_SPIRAM))) {
    sequential(j, "psram_seq", p, 2u << 20);
    j.num("psram_random_16k_ns", randomRead(p, 16 * 1024, kReads) - base, 2);
    j.num("psram_random_2m_ns", randomRead(p, 2u << 20, kReads) - base, 2);
    heap_caps_free(p);
  } else {
    j.raw("psram", "\"no 2 MB block free\"");
  }
  return j.done();
}

std::string flash(const std::string &path) {
  Json j;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    j.raw("error", "\"could not open " + path + "\"");
    return j.done();
  }
  const size_t size = f.size();
  std::vector<uint8_t> buf(4096);
  // Sequential: the first 512 KB in 4 KB reads.
  const size_t span = std::min<size_t>(size, 512 * 1024);
  f.seek(0);
  int64_t t = now();
  for (size_t at = 0; at + buf.size() <= span; at += buf.size()) f.read(buf.data(), buf.size());
  const int64_t seqUs = now() - t;
  j.num("seq_mb_s", double(span) / double(seqUs));
  // Random: a sprite's stream is a seek and a read of a few KB.
  uint32_t x = 777;
  constexpr int kSeeks = 32;
  t = now();
  for (int i = 0; i < kSeeks; ++i) {
    x = x * 1664525u + 1013904223u;
    const size_t at = size > buf.size() ? (x % (size - buf.size())) : 0;
    f.seek(at);
    f.read(buf.data(), buf.size());
  }
  j.num("random_4k_read_ms", double(now() - t) / 1000.0 / kSeeks, 2);
  j.num("file_mb", double(size) / 1048576.0, 1);
  f.close();
  return j.done();
}

}  // namespace

std::string run(const std::string &packPath) {
  Json j;
  j.num("cpu_mhz", getCpuFrequencyMhz(), 0);
  const int64_t t = now();
  j.raw("memory", memory());
  j.raw("ops", ops());
  if (!packPath.empty()) j.raw("flash", flash(packPath));
  j.num("internal_free_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0, 0);
  j.num("internal_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0, 0);
  j.num("psram_free_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0, 0);
  j.num("psram_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024.0, 0);
  j.num("took_ms", double(now() - t) / 1000.0, 0);
  return j.done();
}

}  // namespace birdposter::bench
