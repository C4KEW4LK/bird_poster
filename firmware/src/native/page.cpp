// Host rendering of the real page, from a plate pack (build order step 3's
// pure half): the same lib/render the device runs, writing a PPM instead of
// pushing to glass.
//
//   .pio/build/native/program plates.bin page.ppm [--font f.ttf] [--name-font g.ttf] [--layout N]
//                             [--portrait] [--no-names | --name-style 0-3] [--name-case 0-2] [--count N] [--names a,b]
//                             [--labels birdnet_labels.txt | --no-common] [--vivid 0-4] [--sharpen 0-4] [--edges 0-4] [--no-grow]
//                             [--pack classic|grid|scatter]
//                             [--setup] [--status]
//
// Common names come from a BirdNET label file ("Genus species_Common Name" a
// line; the v2.4 one under assets/ by default) to stand in for what the
// source supplies on the device.
//
// The dither, the labels, the QR code and the status layout are all things
// that are wrong in ways a serial log cannot show, so this is where they are
// looked at.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pages.h"
#include "plates.h"
#include "render.h"

using namespace birdframe;

namespace {

// A binary PPM (P6) or PGM (P5): the pixels after the header, 1 or 3 a pixel.
bool readNetpbm(const char *path, int &w, int &h, int channels, std::vector<uint8_t> &px) {
  std::ifstream in(path, std::ios::binary);
  std::string magic;
  int maxv = 0;
  if (!(in >> magic >> w >> h >> maxv) || maxv != 255) return false;
  if (magic != (channels == 3 ? "P6" : "P5")) return false;
  in.get();
  px.resize(size_t(w) * h * channels);
  return bool(in.read(reinterpret_cast<char *>(px.data()), std::streamsize(px.size())));
}

bool readFile(const char *path, std::vector<uint8_t> &out) {
  std::FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(size_t(std::max(0L, n)));
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool writePpm(const char *path, const Frame &frame) {
  std::FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", frame.w, frame.h);
  std::vector<uint8_t> row(size_t(frame.w) * 3);
  for (int y = 0; y < frame.h; ++y) {
    const uint8_t *src = frame.row(y);
    for (int x = 0; x < frame.w; ++x) std::memcpy(&row[size_t(x) * 3], kInkRgb[src[x]], 3);
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

int pageMain(int argc, char **argv) {
  const char *in = argv[1];
  const char *out = argc > 2 && argv[2][0] != '-' ? argv[2] : "page.ppm";
  std::string fontPath = "../assets/fonts/gentiumbookplus/GentiumBookPlus-Italic.ttf";
  std::string nameFontPath = "../assets/fonts/gould/GouldCondensed-Regular.ttf";
  std::string labelsPath = "../assets/birdnet_labels_v2.4.txt";
  BirdPageSettings settings;
  int count = 10;
  bool setup = false, status = false;
  DateOrder dateOrder = DateOrder::DayFirst;
  std::string spriteRgb, spriteMask;
  std::string canvasOut;
  std::string webSprite;
  bool keepHairlines = true;  // the name font's; as the frame draws it
  std::vector<std::string> wanted;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--font") && i + 1 < argc) fontPath = argv[++i];
    if (!std::strcmp(argv[i], "--name-font") && i + 1 < argc) nameFontPath = argv[++i];
    if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) settings.variant = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--count") && i + 1 < argc) count = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--portrait")) settings.portrait = true;
    if (!std::strcmp(argv[i], "--no-names")) settings.names = NameStyle::None;
    if (!std::strcmp(argv[i], "--name-case") && i + 1 < argc)
      settings.commonCase = NameCase(std::atoi(argv[++i]) % 3);  // 0 as given, 1 CAPS, 2 lower
    if (!std::strcmp(argv[i], "--name-style") && i + 1 < argc)
      settings.names = NameStyle(std::atoi(argv[++i]) & 3);  // 0 both, 1 scientific, 2 common, 3 none
    if (!std::strcmp(argv[i], "--no-common")) labelsPath.clear();
    if (!std::strcmp(argv[i], "--no-grow")) settings.grow = false;
    if (!std::strcmp(argv[i], "--resample") && i + 1 < argc) {
      const char *v = argv[++i];
      settings.resample = !std::strcmp(v, "bilinear")   ? Resample::Bilinear
                          : !std::strcmp(v, "mitchell") ? Resample::Mitchell
                                                        : Resample::CatmullRom;
    }
    if (!std::strcmp(argv[i], "--sci-percent") && i + 1 < argc)
      settings.subNamePercent = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--jitter") && i + 1 < argc) settings.jitter = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--cream") && i + 1 < argc) settings.cream = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--pack") && i + 1 < argc) {
      const char *v = argv[++i];
      settings.packStyle = !std::strcmp(v, "grid")      ? PackStyle::Grid
                           : !std::strcmp(v, "scatter") ? PackStyle::Scatter
                           : !std::strcmp(v, "hero")    ? PackStyle::Hero
                                                        : PackStyle::Classic;
    }
    // --date STYLE (0..4, see DateStyle) dates the page today, month first
    // after --date-us; --date-pos EDGE ALIGN puts it: t or b, then l, c or r.
    if (!std::strcmp(argv[i], "--no-keep-hairlines")) keepHairlines = false;
    // --sprite RGB.ppm MASK.pgm: draw the first bird from these instead of
    // its pack sprite - a decoded image from a codec experiment - on the
    // pack's own silhouette and layout.
    // --web-sprite FILE.bin: the same, from a sprite as the frame fetches it
    // from the web (export_web_plates.py), through the frame's own decoder.
    if (!std::strcmp(argv[i], "--web-sprite") && i + 1 < argc) webSprite = argv[++i];
    if (!std::strcmp(argv[i], "--sprite") && i + 2 < argc) {
      spriteRgb = argv[++i];
      spriteMask = argv[++i];
    }
    if (!std::strcmp(argv[i], "--canvas") && i + 1 < argc) canvasOut = argv[++i];  // pre-dither PPM
    if (!std::strcmp(argv[i], "--note") && i + 1 < argc) settings.note = argv[++i];
    if (!std::strcmp(argv[i], "--date-us")) dateOrder = DateOrder::MonthFirst;
    if (!std::strcmp(argv[i], "--date") && i + 1 < argc) {
      const std::time_t now = std::time(nullptr);
      std::tm tm{};
      localtime_r(&now, &tm);
      settings.date =
          formatDate(tm, DateStyle(std::clamp(std::atoi(argv[++i]), 0, 4)), dateOrder);
    }
    if (!std::strcmp(argv[i], "--date-pos") && i + 2 < argc) {
      settings.dateEdge = argv[++i][0] == 't' ? DateEdge::Top : DateEdge::Bottom;
      const char a = argv[++i][0];
      settings.dateAlign = a == 'l' ? DateAlign::Left : a == 'c' ? DateAlign::Centre : DateAlign::Right;
    }
    if (!std::strcmp(argv[i], "--vivid") && i + 1 < argc) settings.vivid = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--sharpen") && i + 1 < argc) settings.sharpen = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--edges") && i + 1 < argc) settings.edges = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--labels") && i + 1 < argc) labelsPath = argv[++i];
    if (!std::strcmp(argv[i], "--setup")) setup = true;
    if (!std::strcmp(argv[i], "--status")) status = true;
    if (!std::strcmp(argv[i], "--names") && i + 1 < argc) {
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        wanted.push_back(list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
  }

  std::vector<uint8_t> fontBytes;
  Font font;
  if (!readFile(fontPath.c_str(), fontBytes) || !font.load(std::move(fontBytes))) {
    std::fprintf(stderr, "cannot load font %s\n", fontPath.c_str());
    return 1;
  }

  Font nameFont;
  std::vector<uint8_t> nameBytes;
  if (!nameFontPath.empty() &&
      (!readFile(nameFontPath.c_str(), nameBytes) || !nameFont.load(std::move(nameBytes))))
    std::fprintf(stderr, "no name font at %s - common names in the label font\n", nameFontPath.c_str());
  nameFont.setKeepHairlines(keepHairlines);
  if (!canvasOut.empty()) {
    settings.beforeDither = [canvasOut](const Canvas &c) {
      FILE *f = std::fopen(canvasOut.c_str(), "wb");
      if (!f) return;
      std::fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
      std::vector<uint8_t> row(size_t(c.w) * 3);
      for (int y = 0; y < c.h; ++y) {
        const uint16_t *src = c.row(y);
        for (int x = 0; x < c.w; ++x) {
          int r, g, b;
          rgb888(src[x], r, g, b);
          row[size_t(x) * 3] = uint8_t(r);
          row[size_t(x) * 3 + 1] = uint8_t(g);
          row[size_t(x) * 3 + 2] = uint8_t(b);
        }
        std::fwrite(row.data(), 1, row.size(), f);
      }
      std::fclose(f);
    };
  }
  if (!webSprite.empty()) {
    settings.spriteOverride = [webSprite](size_t index, SpriteImage &out) {
      std::vector<uint8_t> bytes;
      if (index != 0 || !readFile(webSprite.c_str(), bytes)) return false;
      std::string error;
      const bool ok = Plates::decodeSingle(std::string(bytes.begin(), bytes.end()), out, &error);
      std::fprintf(stderr, "web sprite %s: %s\n", webSprite.c_str(),
                   ok ? (std::to_string(out.w) + "x" + std::to_string(out.h)).c_str() : error.c_str());
      return ok;
    };
  }
  if (!spriteRgb.empty()) {
    settings.spriteOverride = [spriteRgb, spriteMask](size_t index, SpriteImage &out) {
      if (index != 0) return false;
      int w = 0, h = 0, mw = 0, mh = 0;
      std::vector<uint8_t> rgb, mask;
      if (!readNetpbm(spriteRgb.c_str(), w, h, 3, rgb) ||
          !readNetpbm(spriteMask.c_str(), mw, mh, 1, mask) || mw != w || mh != h) {
        std::fprintf(stderr, "cannot read --sprite %s %s\n", spriteRgb.c_str(), spriteMask.c_str());
        return false;
      }
      out.w = w;
      out.h = h;
      const int stride = (w + 7) / 8;
      out.paint.assign(size_t(stride) * h, 0);
      out.direct.resize(size_t(w) * h);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const size_t i = size_t(y) * w + x;
          out.direct[i] = rgb565(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
          if (mask[i] >= 128) out.paint[size_t(y) * stride + (x >> 3)] |= uint8_t(0x80 >> (x & 7));
        }
      return true;
    };
  }
  settings.commonFont = &nameFont;

  Frame frame;
  if (setup) {
    renderSetupPage(settings.portrait, font, "birdframe-3A7F", "birdframe", "http://192.168.4.1/",
                    frame);
  } else if (status) {
    renderStatusPage(settings.portrait, font, "Bird frame status",
                     {"WiFi: joined HomeNet as 192.168.1.42 (-61 dBm)", "Settings: http://192.168.1.42/",
                      "Source: iNaturalist, most seen, 25 km around -27.4700, 153.0200, 30 days",
                      "Endpoint: https://api.inaturalist.org/v1/observations/species_counts",
                      "!Last fetch: failed - HTTP 0 (no route)", "Last page: 10 birds, 2026-09-15 08:00",
                      "Refresh: every 60 min, next 09:00", "Plates: au, 548 species",
                      "Battery: not measured", "Firmware: 3f994b6"},
                     frame);
  } else {
    std::vector<uint8_t> pack;
    if (!readFile(in, pack)) {
      std::fprintf(stderr, "cannot read %s\n", in);
      return 1;
    }
    Plates plates;
    std::string err;
    if (!plates.open(
            [&pack](uint32_t offset, void *dst, size_t len) {
              if (size_t(offset) + len > pack.size()) return false;
              std::memcpy(dst, pack.data() + offset, len);
              return true;
            },
            &err)) {
      std::fprintf(stderr, "%s: %s\n", in, err.c_str());
      return 1;
    }
    std::vector<int> indices;
    if (!wanted.empty()) {
      for (const std::string &n : wanted) {
        const int idx = plates.find(n);
        if (idx < 0) std::fprintf(stderr, "no plate for %s\n", n.c_str());
        else indices.push_back(idx);
      }
    } else {
      for (size_t i = 0; i < plates.count() && int(i) < count; ++i) indices.push_back(int(i));
    }
    if (!labelsPath.empty()) {
      std::map<std::string, std::string> common;
      std::ifstream in(labelsPath);
      if (!in) std::fprintf(stderr, "no label file %s - scientific names only\n", labelsPath.c_str());
      for (std::string line; std::getline(in, line);) {
        const size_t us = line.find('_');
        if (us != std::string::npos) common[line.substr(0, us)] = line.substr(us + 1);
      }
      for (int idx : indices) {
        const auto it = common.find(plates.entry(size_t(idx)).name);
        settings.commonNames.push_back(it == common.end() ? std::string() : it->second);
      }
    }
    BirdPageReport report;
    if (!renderBirdPage(plates, indices, settings, font, frame, &report)) {
      std::fprintf(stderr, "no layout fits\n");
      return 2;
    }
    std::printf("%d birds: pack %d ms, draw %d ms, dither %d ms; labels %d px, median bird %d px\n",
                report.placed, report.packMs, report.drawMs, report.ditherMs, report.labelPx,
                report.medianDim);
    for (int idx : indices) std::printf("  %s\n", plates.entry(size_t(idx)).name.c_str());
  }
  if (!writePpm(out, frame)) {
    std::fprintf(stderr, "cannot write %s\n", out);
    return 3;
  }
  std::printf("wrote %s (%dx%d)\n", out, frame.w, frame.h);
  return 0;
}
