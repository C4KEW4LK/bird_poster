// Where the frame gets its birds from.
//
// Five sources, chosen by the user in the settings portal, because they answer
// different questions and none is strictly better:
//
//   BirdNET-Go   what a microphone actually heard at this address, minute by
//                minute. Needs one running on the LAN.
//   iNaturalist  what people have recorded near a latitude and longitude. No
//                hardware at all, and it works anywhere.
//   eBird        what birders have reported near a latitude and longitude, in
//                the last 30 days. Densest where iNaturalist is thin, and the
//                one source with a real "notable" list. Needs a free key.
//   ALA          the Atlas of Living Australia: every occurrence record for
//                Australia, eBird's and BirdLife's included, so for an
//                Australian frame it is the fullest local list there is.
//   a JSON list  any URL that answers with a list of scientific names: a
//                Home Assistant automation, a Bird Buddy export, a cron job
//                beside a BirdNET-Pi. The escape hatch for everything else.
//
// All public HTTP; only eBird wants a key, and it is free and instant.
//
// Nothing here touches Arduino or WiFi. Building the URL, parsing the reply and
// choosing the page are pure functions over strings, so the whole source layer
// is testable on the host against canned JSON - the same reason lib/packer is
// Arduino-free. `fetch()` is the only part that needs a radio, and it is passed
// in rather than called, so `pio test -e native` exercises everything else.
//
// See notes/esp32-port/IMPLEMENTATION.md §6.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace birdposter {

enum class Source {
  BirdNet,      // a BirdNET-Go instance on the LAN
  iNaturalist,  // api.inaturalist.org
  eBird,        // api.ebird.org, with the owner's key
  Ala,          // biocache-ws.ala.org.au
  JsonList,     // a URL of the owner's own that answers with names
};

// What the settings page calls it.
const char* sourceName(Source source);

// How the page ranks what it found.
enum class Mode {
  MostDetected,  // the birds seen most here
  Rarest,        // the birds that are rare *in the world*, seen here
};

// iNaturalist asks for a descriptive User-Agent rather than a key. Sending the
// repo rather than a person keeps a contact in the header without putting an
// address in the firmware image.
constexpr const char* kUserAgent =
    "birdposter/0.1 (+https://github.com/C4KEW4LK/bird_poster)";

// Aves. iNaturalist's own taxon id, not something we assign.
constexpr int kTaxonAves = 3;

// One species, from either source. The two fill it differently, and the gap is
// the point: see `globalCount`.
struct Sighting {
  std::string scientific;  // the artwork key
  std::string common;      // label text; may be empty
  int localCount = 0;      // sightings or detections here
  // Observations worldwide. iNaturalist reports it; BirdNET-Go has no idea, and
  // leaves it 0 - which is why `Rarest` is not offered for that source rather
  // than silently ranked on something else. See `supports`.
  long globalCount = 0;
};

struct SourceConfig {
  Source source = Source::iNaturalist;

  // BirdNET-Go: the base URL of the instance, no trailing slash. Its HTTP API is
  // v2 whatever BirdNET model it runs - the API version and the model version
  // are unrelated, and there is no v3 of either path.
  std::string detectorUrl = "http://birdnet-go.local:8080";

  // iNaturalist: where to look.
  double lat = 0.0;
  double lng = 0.0;
  int radiusKm = 25;

  // How far back, as the earliest moment a sighting may have; 0 for no
  // window. iNaturalist takes it as a UTC datetime in the request, so it is
  // good to the minute. BirdNET-Go's recent-detections endpoint has no
  // window, so it is applied to the reply instead - see parseResponse.
  std::time_t since = 0;

  // Which iNaturalist API. v2 takes a `fields` list and answers with only
  // that - 17 KB for Canberra where v1 sends 145 - but iNaturalist still calls
  // it in development and may change it; v1 is the frozen, documented one.
  // Both answer the same shape for what is read here, so only the URL
  // differs, and the user can fall back without a reflash.
  int inatVersion = 2;

  // Species to ask for. More than the page needs, because most of them will
  // have no artwork and be dropped.
  int limit = 200;

  // eBird: the owner's API key, sent as a header. Free from
  // https://ebird.org/api/keygen; the frame only reads.
  std::string ebirdKey;
  // Which English eBird writes the common names in. Plain "en" is the
  // Clements checklist's - "Maned Duck", "Gray Teal" - and that is what would
  // go under the bird; "en_AU" says Australian Wood Duck and Grey Teal.
  std::string ebirdLocale = "en";

  // JSON list: the URL to GET. It may answer with a bare array of scientific
  // names, or an array of objects carrying `scientific` (or BirdNET-Go's
  // `scientificName`), and optionally `common` and `count`.
  std::string listUrl;
};

// Headers a request needs beyond the User-Agent: eBird's key. Empty for the
// rest, so the fetch code has one path.
std::vector<std::pair<std::string, std::string>> requestHeaders(const SourceConfig& config);

// Whether a source can answer this mode at all. Rarest needs a global
// observation count, which only iNaturalist has: ranking by fewest *local*
// sightings looks like rarity and is not - in Bergen it puts Mallard (953,735
// observations worldwide) above Tawny Owl (23,627), purely because one person
// logged it once. A frame set to Rarest on BirdNET-Go should say so, not draw a
// page that quietly means something else. eBird is the exception: it has no
// global count either, but it has a *notable* list - sightings its own
// regional filters flag as unusual for the place and the date - which is
// rarity as a birder would judge it, and that is what its Rarest asks for.
bool supports(Source source, Mode mode);

// The URL to GET. `now` is the device's clock, used for the lookback window
// where a source takes one as a count of days back rather than an instant
// (eBird); pass a fixed value to test it. `mode` matters to eBird alone, whose
// Rarest is a different endpoint.
std::string requestUrl(const SourceConfig& config, std::time_t now, Mode mode = Mode::MostDetected);

// Parse a reply body into sightings, merging repeats. Returns false if the body
// is not the shape this source is documented to return, leaving `out` alone -
// a half-parsed page is worse than the one already on the glass. `why`, if
// given, gets one line saying what was wrong.
//
// `sinceLocal` applies the window to BirdNET-Go, whose detections carry a
// local date and time: "YYYY-MM-DD HH:MM:SS", and a detection earlier than it
// is dropped. Empty means no window. The caller formats it in the frame's own
// timezone, which is the detector's too, since they share a house.
//
// BirdNET-Go returns a flat array of detections, one per call of one bird, so
// the same species arrives many times and `localCount` counts them.
// iNaturalist and ALA return species already counted. eBird returns one row
// per species, its most recent report, so `localCount` is that report's
// `howMany` and the order is recency. A JSON list is taken in the order given.
bool parseResponse(Source source, const std::string& body, std::vector<Sighting>& out,
                   std::string* why = nullptr, const std::string& sinceLocal = "");

// One line for a reply that was not a 200, in the source's own words where
// it gave any. eBird answers a bad request with {"errors":[{"title":...}]}
// and a bad key with an empty 403; the rest mostly send HTML, which is left
// unread. Always begins "HTTP <status>".
std::string explainStatus(Source source, int http, const std::string& body);

// What to suggest when the source answered and named no bird at all. The
// place-based sources have a radius and a window to widen; ALA covers one
// country and is the source most likely to be asked about the wrong one.
std::string emptyReplyHint(Source source, const SourceConfig& config);

// The birds to draw, in the order the page wants them.
//
// `drawable` answers whether artwork exists for a scientific name. It is applied
// *before* ranking, never after: rank first and the rare mode picks its birds,
// then discards most of them for having no illustration, and the page ends up
// with two.
std::vector<Sighting> choose(const std::vector<Sighting>& seen,
                             const std::function<bool(const std::string&)>& drawable,
                             Mode mode, std::size_t limit);

// Rotate through the birds instead of drawing the same top few every time.
//
// `ranked` is choose()'s full list; `lastShown` says when a species was last
// on the glass (0 for never). Species not shown within `windowHours` are the
// pool, and the page is a random `limit` of them, `seed` deciding which - so
// over the window every drawable bird gets its turn. When the pool runs
// short the page is topped up with the longest-unseen of the rest, so a
// window longer than the list is long merely becomes "least recently shown".
// The ranking survives only as the order within each group.
std::vector<Sighting> cycle(const std::vector<Sighting>& ranked,
                            const std::function<std::time_t(const std::string&)>& lastShown,
                            std::time_t now, int windowHours, std::size_t limit, uint32_t seed);

}  // namespace birdposter
