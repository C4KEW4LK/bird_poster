#include "source.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>

namespace birdposter {
namespace {

std::string trimSlash(const std::string& url) {
  std::size_t end = url.size();
  while (end > 0 && url[end - 1] == '/') --end;
  return url.substr(0, end);
}

// An instant as iNaturalist's d1 takes it: an ISO datetime in UTC, which it
// honours to the second (a date alone would round the window to midnight).
std::string isoUtc(std::time_t t) {
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  // Wide enough that the compiler can prove no truncation: tm fields are plain
  // ints, so it has to assume the year could be any of them.
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

// %g would drop precision on a coordinate and %f pads it; six decimals is about
// 10 cm, which is far finer than a radius measured in kilometres.
std::string coordinate(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6f", value);
  return buf;
}

// A query value with the characters a URL cannot carry raw escaped. ALA's
// filter syntax is Solr - "occurrence_date:[2026-08-01T00:00:00Z TO *]" - and
// the brackets and the space in it are what break an unescaped request.
std::string encoded(const std::string& value) {
  std::string out;
  for (unsigned char c : value) {
    const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out.push_back(char(c));
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// eBird takes the window as whole days back, 1 to 30; anything longer is
// simply its limit, and no window at all asks for everything it has.
int daysBack(std::time_t since, std::time_t now) {
  if (since <= 0 || now <= since) return 30;
  const long days = (long(now - since) + 86399) / 86400;
  return int(days < 1 ? 1 : (days > 30 ? 30 : days));
}

void accumulate(std::vector<Sighting>& out, const std::string& scientific,
                const std::string& common, int count, long global) {
  if (scientific.empty()) return;
  for (auto& seen : out) {
    if (seen.scientific == scientific) {
      seen.localCount += count;
      if (seen.common.empty()) seen.common = common;
      if (global > seen.globalCount) seen.globalCount = global;
      return;
    }
  }
  out.push_back(Sighting{scientific, common, count, global});
}

// What a parse failure means for the caller's message. Anything but a shape
// problem is reported as itself: a reply that is too big to hold is a
// different fix from one that changed shape, and the board only ever sees the
// first kind.
bool failed(DeserializationError err, std::string* why) {
  if (err == DeserializationError::Ok) return false;
  if (why) {
    if (err == DeserializationError::NoMemory) *why = "reply too large to parse";
    else if (err == DeserializationError::EmptyInput) *why = "empty reply";
    else *why = std::string("reply was not JSON: ") + err.c_str();
  }
  return true;
}

// Only the fields read are kept. Unfiltered, an iNaturalist page of 126
// species costs 232 KB of heap - each taxon carries its ancestry, photo and
// flag counts - which is more than the board has left once WiFi and TLS are
// up. Filtered it is 33 KB.
bool parseBirdNet(const std::string& body, std::vector<Sighting>& out, std::string* why,
                  const std::string& sinceLocal) {
  JsonDocument filter;
  filter[0]["scientificName"] = true;
  filter[0]["commonName"] = true;
  filter[0]["date"] = true;
  filter[0]["time"] = true;
  JsonDocument doc;
  if (failed(deserializeJson(doc, body, DeserializationOption::Filter(filter)), why)) return false;
  // GET /api/v2/detections/recent answers with a bare array, not an envelope.
  JsonArrayConst rows = doc.as<JsonArrayConst>();
  if (rows.isNull()) {
    if (why) *why = "reply was not the shape expected";
    return false;
  }
  std::vector<Sighting> parsed;
  for (JsonObjectConst row : rows) {
    // One row is one call of one bird, so a species repeats and the repeats are
    // the count. No global total exists here at all.
    if (!sinceLocal.empty()) {
      // "2026-09-06" and "07:14:02" compare as text against the window.
      const std::string when = std::string(row["date"] | "") + " " + (row["time"] | "");
      if (when < sinceLocal) continue;
    }
    accumulate(parsed, row["scientificName"] | "", row["commonName"] | "", 1, 0);
  }
  out.swap(parsed);
  return true;
}

// One row per species, its most recent report; `howMany` is that report's
// flock size, absent when the observer did not count. The notable endpoint
// answers the same shape, one row per report rather than per species, which
// `accumulate` folds together.
bool parseEBird(const std::string& body, std::vector<Sighting>& out, std::string* why) {
  JsonDocument filter;
  filter[0]["sciName"] = true;
  filter[0]["comName"] = true;
  filter[0]["howMany"] = true;
  JsonDocument doc;
  if (failed(deserializeJson(doc, body, DeserializationOption::Filter(filter)), why)) return false;
  JsonArrayConst rows = doc.as<JsonArrayConst>();
  if (rows.isNull()) {
    if (why) *why = "reply was not the shape expected";
    return false;
  }
  std::vector<Sighting> parsed;
  for (JsonObjectConst row : rows) {
    accumulate(parsed, row["sciName"] | "", row["comName"] | "", row["howMany"] | 1, 0);
  }
  out.swap(parsed);
  return true;
}

// A facet over species: {"facetResults":[{"fieldName":"species","fieldResult":
// [{"label":"Malurus cyaneus","count":75,"fq":"species:\"Malurus cyaneus\""},
// ...]}]}. Records with no species come back as a bucket labelled "Not
// supplied" whose fq is a negation, "-species:*" - the fq is what says it is
// not a name, so that is what is tested, and a label that is not a binomial
// is skipped too rather than sent to the plate lookup.
bool parseAla(const std::string& body, std::vector<Sighting>& out, std::string* why) {
  JsonDocument filter;
  filter["facetResults"][0]["fieldResult"][0]["label"] = true;
  filter["facetResults"][0]["fieldResult"][0]["count"] = true;
  filter["facetResults"][0]["fieldResult"][0]["fq"] = true;
  JsonDocument doc;
  if (failed(deserializeJson(doc, body, DeserializationOption::Filter(filter)), why)) return false;
  JsonArrayConst facets = doc["facetResults"].as<JsonArrayConst>();
  if (facets.isNull()) {
    if (why) *why = "reply was not the shape expected";
    return false;
  }
  std::vector<Sighting> parsed;
  for (JsonObjectConst facet : facets) {
    for (JsonObjectConst row : facet["fieldResult"].as<JsonArrayConst>()) {
      const std::string label = row["label"] | "";
      const std::string fq = row["fq"] | "";
      if (!fq.empty() && fq[0] == '-') continue;
      const std::size_t space = label.find(' ');
      if (space == std::string::npos || label.find(' ', space + 1) != std::string::npos) continue;
      if (!std::isupper(static_cast<unsigned char>(label[0]))) continue;
      accumulate(parsed, label, "", row["count"] | 0, 0);
    }
  }
  out.swap(parsed);
  return true;
}

// The owner's own list. A bare array of names, or an array of objects with
// `scientific` (BirdNET-Go's `scientificName` is taken too, so a detector's
// own export drops straight in), and optionally `common` and `count`. An
// object at the top is searched for the first array in it, so {"birds":[...]}
// works without being documented as the shape. The order given is the
// ranking where there are no counts: a list is already someone's choice.
bool parseList(const std::string& body, std::vector<Sighting>& out, std::string* why) {
  JsonDocument doc;
  if (failed(deserializeJson(doc, body), why)) return false;
  JsonArrayConst rows = doc.as<JsonArrayConst>();
  if (rows.isNull()) {
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
      if (kv.value().is<JsonArrayConst>()) {
        rows = kv.value().as<JsonArrayConst>();
        break;
      }
    }
  }
  if (rows.isNull()) {
    if (why) *why = "reply was not a list";
    return false;
  }
  std::vector<Sighting> parsed;
  for (JsonVariantConst row : rows) {
    if (row.is<const char*>()) {
      accumulate(parsed, row.as<const char*>(), "", 1, 0);
    } else if (row.is<JsonObjectConst>()) {
      JsonObjectConst o = row.as<JsonObjectConst>();
      const char* sci = o["scientific"] | (o["scientificName"] | "");
      const char* common = o["common"] | (o["commonName"] | "");
      accumulate(parsed, sci, common, o["count"] | 1, o["global"] | 0L);
    }
  }
  out.swap(parsed);
  return true;
}

bool parseINaturalist(const std::string& body, std::vector<Sighting>& out, std::string* why) {
  JsonDocument filter;
  filter["results"][0]["count"] = true;
  JsonObject taxon = filter["results"][0]["taxon"].to<JsonObject>();
  taxon["name"] = true;
  taxon["preferred_common_name"] = true;
  taxon["observations_count"] = true;
  JsonDocument doc;
  if (failed(deserializeJson(doc, body, DeserializationOption::Filter(filter)), why)) return false;
  JsonArrayConst rows = doc["results"].as<JsonArrayConst>();
  if (rows.isNull()) {
    if (why) *why = "reply was not the shape expected";
    return false;
  }
  std::vector<Sighting> parsed;
  for (JsonObjectConst row : rows) {
    JsonObjectConst taxon = row["taxon"];
    if (taxon.isNull()) continue;
    accumulate(parsed, taxon["name"] | "", taxon["preferred_common_name"] | "",
               row["count"] | 0, taxon["observations_count"] | 0L);
  }
  out.swap(parsed);
  return true;
}

}  // namespace

const char* sourceName(Source source) {
  switch (source) {
    case Source::BirdNet: return "BirdNET-Go";
    case Source::iNaturalist: return "iNaturalist";
    case Source::eBird: return "eBird";
    case Source::Ala: return "Atlas of Living Australia";
    case Source::JsonList: return "JSON list";
  }
  return "?";
}

bool supports(Source source, Mode mode) {
  if (mode == Mode::Rarest) return source == Source::iNaturalist || source == Source::eBird;
  return true;
}

std::vector<std::pair<std::string, std::string>> requestHeaders(const SourceConfig& config) {
  std::vector<std::pair<std::string, std::string>> headers;
  if (config.source == Source::eBird) headers.emplace_back("X-eBirdApiToken", config.ebirdKey);
  return headers;
}

std::string requestUrl(const SourceConfig& config, std::time_t now, Mode mode) {
  if (config.source == Source::BirdNet) {
    // Public unless the detector is in private mode, which `explainStatus`
    // says more about. The frame only reads; nothing here can change a
    // setting on the detector or delete a recording.
    return trimSlash(config.detectorUrl) + "/api/v2/detections/recent?limit=" +
           std::to_string(config.limit);
  }
  if (config.source == Source::JsonList) return config.listUrl;
  if (config.source == Source::eBird) {
    // Recent observations within `dist` km (50 at most) over `back` days (30
    // at most): one row per species. Rarest asks the notable endpoint, which
    // lists what eBird's own regional filters flagged as unusual. The key
    // travels as a header, not in the URL, so it is not in any log line.
    const int dist = config.radiusKm > 50 ? 50 : (config.radiusKm < 1 ? 1 : config.radiusKm);
    return std::string("https://api.ebird.org/v2/data/obs/geo/recent") +
           (mode == Mode::Rarest ? "/notable" : "") + "?lat=" + coordinate(config.lat) +
           "&lng=" + coordinate(config.lng) + "&dist=" + std::to_string(dist) +
           "&back=" + std::to_string(daysBack(config.since, now)) +
           "&maxResults=" + std::to_string(config.limit) +
           "&sppLocale=" + encoded(config.ebirdLocale.empty() ? "en" : config.ebirdLocale);
  }
  if (config.source == Source::Ala) {
    // A facet over species, and no records: pageSize=0 keeps the reply to the
    // counts, 26 KB for 200 species where the records themselves would be
    // megabytes. Birds only, within the radius, since the window; counts
    // descending so the first `limit` are the most recorded.
    return std::string("https://biocache-ws.ala.org.au/ws/occurrences/search?q=") +
           encoded("*:*") + "&fq=" + encoded("class:Aves") +
           (config.since > 0 ? "&fq=" + encoded("occurrence_date:[" + isoUtc(config.since) + " TO *]")
                             : std::string()) +
           "&lat=" + coordinate(config.lat) + "&lon=" + coordinate(config.lng) +
           "&radius=" + std::to_string(config.radiusKm) +
           "&facets=species&fsort=count&flimit=" + std::to_string(config.limit) + "&pageSize=0";
  }
  // Both page modes come from this one call: it returns every species in the
  // radius with a local count and a global one.
  const bool v2 = config.inatVersion == 2;
  return std::string("https://api.inaturalist.org/") + (v2 ? "v2" : "v1") +
         "/observations/species_counts"
         "?taxon_id=" +
         std::to_string(kTaxonAves) + "&lat=" + coordinate(config.lat) +
         "&lng=" + coordinate(config.lng) + "&radius=" + std::to_string(config.radiusKm) +
         (config.since > 0 ? "&d1=" + isoUtc(config.since) : std::string()) +
         "&quality_grade=research&per_page=" + std::to_string(config.limit) +
         (v2 ? "&fields=count,taxon.name,taxon.preferred_common_name,taxon.observations_count"
             : "");
}

bool parseResponse(Source source, const std::string& body, std::vector<Sighting>& out,
                   std::string* why, const std::string& sinceLocal) {
  switch (source) {
    case Source::BirdNet: return parseBirdNet(body, out, why, sinceLocal);
    case Source::iNaturalist: return parseINaturalist(body, out, why);
    case Source::eBird: return parseEBird(body, out, why);
    case Source::Ala: return parseAla(body, out, why);
    case Source::JsonList: return parseList(body, out, why);
  }
  return false;
}

namespace {

// The message a service put in its error body, if it put one where this
// looks. Each has its own shape, all of them small:
//   eBird        {"errors":[{"title":"Field dist of rawDataCmd: Distance must be between 0 and 50"}]}
//   iNaturalist  {"error":"Elasticsearch error, if this persists ...","status":500}
//   ALA          {"message":"Error from server at null: Invalid Date String:'nonsense'","errorType":"Query syntax invalid"}
//   BirdNET-Go   {"error":"...","message":"...","code":400}
// and a 404 from any of them is a page of HTML, which is not a message.
std::string serviceMessage(const std::string& body) {
  if (body.empty() || body[0] != '{') return "";
  JsonDocument filter;
  filter["errors"][0]["title"] = true;
  filter["errors"][0]["message"] = true;
  filter["error"] = true;
  filter["message"] = true;
  filter["errorType"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return "";
  std::string text = doc["errors"][0]["title"] | (doc["errors"][0]["message"] | "");
  if (text.empty()) text = doc["message"] | "";
  if (text.empty() && doc["error"].is<const char*>()) text = doc["error"].as<const char*>();
  // "Field dist of rawDataCmd: Distance must be between 0 and 50" - the half
  // after the colon is the part written for a person.
  if (text.rfind("Field ", 0) == 0) {
    const std::size_t colon = text.find(": ");
    if (colon != std::string::npos) text = text.substr(colon + 2);
  }
  // iNaturalist's bare "Error" says nothing; ALA's errorType adds the class.
  if (text == "Error") text.clear();
  const char* type = doc["errorType"] | "";
  if (*type && !text.empty()) text = std::string(type) + ": " + text;
  return text;
}

}  // namespace

std::string explainStatus(Source source, int http, const std::string& body) {
  const std::string line = "HTTP " + std::to_string(http);
  const std::string name = sourceName(source);
  const std::string said = serviceMessage(body);
  const std::string says = said.empty() ? "" : " - " + name + " says: " + said;
  switch (http) {
    case 400:
    case 422:
      return line + (said.empty() ? " - " + name + " rejected the request; check the place and the radius" : says);
    case 401:
    case 403:
      if (source == Source::eBird)
        return line + " - eBird refused the API key; check it, or make one at ebird.org/api/keygen";
      // Detections are public by default and private mode is the one setting
      // that closes them, so that is what a refusal means. The frame has no
      // credential it can send, so the way through is on the detector.
      if (source == Source::BirdNet)
        return line +
               " - BirdNET-Go refused the request; it is in private mode. The frame cannot sign "
               "in, so allow its network under the detector's Security > Subnet bypass" +
               says;
      return line + " - " + name + " refused the request" + says;
    case 404:
      return line + " - nothing at that path" +
             (source == Source::BirdNet ? "; is this a BirdNET-Go instance, and is the port right?"
              : source == Source::JsonList ? "; check the list URL" : "") + says;
    case 429:
      return line + " - " + name + " is rate limiting" +
             (source == Source::eBird ? "; the frame asks once an hour, so something else is using this key" : "; the next refresh will try again") + says;
    default:
      if (http >= 500)
        return line + " - " + name + " is having trouble; the next refresh will try again" + says;
      return line + says;
  }
}

std::string emptyReplyHint(Source source, const SourceConfig& config) {
  switch (source) {
    case Source::BirdNet: return "has it detected any birds yet?";
    case Source::JsonList: return "is the list empty?";
    case Source::eBird:
      return "no eBird reports within " + std::to_string(config.radiusKm > 50 ? 50 : config.radiusKm) +
             " km in the last " + std::to_string(daysBack(config.since, std::time(nullptr))) +
             " days - eBird allows up to 50 km and 30 days, so widen one of them, or check the "
             "place: a coordinate at sea has no birds to report";
    case Source::Ala:
      return "no ALA records here - the Atlas of Living Australia covers Australia and its "
             "territories only; for anywhere else use iNaturalist or eBird. If the place is in "
             "Australia, try a larger radius or more days";
    case Source::iNaturalist: return "try a larger radius or more days";
  }
  return "";
}

std::vector<Sighting> choose(const std::vector<Sighting>& seen,
                             const std::function<bool(const std::string&)>& drawable,
                             Mode mode, std::size_t limit) {
  std::vector<Sighting> page;
  page.reserve(seen.size());
  for (const Sighting& bird : seen) {
    if (!drawable || drawable(bird.scientific)) page.push_back(bird);
  }

  if (mode == Mode::Rarest) {
    // Ascending global count, and a species with no global figure sorts last
    // rather than first - a 0 there means "this source did not say", not "never
    // recorded anywhere", and the rarest page must not be led by ignorance.
    std::stable_sort(page.begin(), page.end(), [](const Sighting& a, const Sighting& b) {
      const bool aKnown = a.globalCount > 0, bKnown = b.globalCount > 0;
      if (aKnown != bKnown) return aKnown;
      return a.globalCount < b.globalCount;
    });
  } else {
    std::stable_sort(page.begin(), page.end(),
                     [](const Sighting& a, const Sighting& b) { return a.localCount > b.localCount; });
  }

  if (page.size() > limit) page.resize(limit);
  return page;
}

std::vector<Sighting> cycle(const std::vector<Sighting>& ranked,
                            const std::function<std::time_t(const std::string&)>& lastShown,
                            std::time_t now, int windowHours, std::size_t limit, uint32_t seed) {
  const std::time_t since = now - static_cast<std::time_t>(windowHours) * 60 * 60;
  std::vector<Sighting> fresh, stale;
  std::vector<std::time_t> staleAt;
  for (const Sighting& bird : ranked) {
    const std::time_t at = lastShown ? lastShown(bird.scientific) : 0;
    if (at == 0 || at < since) {
      fresh.push_back(bird);
    } else {
      stale.push_back(bird);
      staleAt.push_back(at);
    }
  }
  std::shuffle(fresh.begin(), fresh.end(), std::mt19937(seed));
  if (fresh.size() > limit) fresh.resize(limit);
  if (fresh.size() < limit && !stale.empty()) {
    // Oldest first, ranking as the tie-break (stable).
    std::vector<std::size_t> order(stale.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&staleAt](std::size_t a, std::size_t b) { return staleAt[a] < staleAt[b]; });
    for (std::size_t i : order) {
      if (fresh.size() >= limit) break;
      fresh.push_back(stale[i]);
    }
  }
  return fresh;
}

}  // namespace birdposter
