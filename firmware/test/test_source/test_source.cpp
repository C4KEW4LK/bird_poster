// Source layer, on the host. Canned replies rather than a radio: the point of
// keeping the URL, the parse and the ranking pure is that they can be wrong
// here, in a second, instead of on a board with a 30-second panel refresh.
//
// The bodies below are trimmed from the shapes the APIs document: BirdNET-Go's
// /api/v2/detections/recent answers with a bare array of detections,
// iNaturalist's species_counts with {"results": [...]}, eBird's obs/geo/recent
// with a bare array of reports, ALA's occurrences/search with facetResults,
// and a JSON list is whatever the owner's script wrote.
#include <unity.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "source.h"

using namespace birdframe;

namespace {

// 2026-09-06T00:00:00Z, so the lookback window is arithmetic rather than "now".
constexpr std::time_t kNow = 1788652800;

const char* kBirdNetBody = R"([
  {"id":3,"date":"2026-09-06","time":"07:14:02","scientificName":"Turdus merula",
   "commonName":"Eurasian Blackbird","confidence":0.91},
  {"id":2,"date":"2026-09-06","time":"07:02:11","scientificName":"Turdus merula",
   "commonName":"Eurasian Blackbird","confidence":0.77},
  {"id":1,"date":"2026-09-06","time":"06:58:40","scientificName":"Strix aluco",
   "commonName":"Tawny Owl","confidence":0.64}
])";

const char* kINatBody = R"({"total_results":3,"results":[
  {"count":42,"taxon":{"name":"Anas platyrhynchos","preferred_common_name":"Mallard",
   "observations_count":953735}},
  {"count":9,"taxon":{"name":"Strix aluco","preferred_common_name":"Tawny Owl",
   "observations_count":23627}},
  {"count":3,"taxon":{"name":"Turdus merula","preferred_common_name":"Eurasian Blackbird",
   "observations_count":412000}}
]})";

const char* kEBirdBody = R"([
  {"speciesCode":"eursta","comName":"European Starling","sciName":"Sturnus vulgaris",
   "locId":"L1","obsDt":"2026-09-05 17:02","howMany":40,"lat":-35.28,"lng":149.13},
  {"speciesCode":"tawowl1","comName":"Tawny Owl","sciName":"Strix aluco",
   "locId":"L2","obsDt":"2026-09-04 21:10","lat":-35.29,"lng":149.12},
  {"speciesCode":"eurbla","comName":"Eurasian Blackbird","sciName":"Turdus merula",
   "locId":"L1","obsDt":"2026-09-05 07:40","howMany":3}
])";

// As biocache-ws answered for Canberra, trimmed: facets come sorted by count
// and the records with no species land under "Not supplied".
const char* kAlaBody = R"({"pageSize":0,"startIndex":0,"totalRecords":1546,"sort":"score",
 "dir":"asc","status":"OK","facetResults":[{"fieldName":"species","fieldResult":[
  {"label":"Chenonetta jubata","i18nCode":"species.Chenonetta jubata","count":78,
   "fq":"species:\"Chenonetta jubata\""},
  {"label":"Malurus cyaneus","i18nCode":"species.Malurus cyaneus","count":75,
   "fq":"species:\"Malurus cyaneus\""},
  {"label":"Not supplied","i18nCode":"species.Not supplied","count":13,"fq":"-species:*"},
  {"label":"Strix aluco","i18nCode":"species.Strix aluco","count":2,"fq":"species:\"Strix aluco\""}
 ]}],"occurrences":[]})";

const Sighting* find(const std::vector<Sighting>& all, const std::string& name) {
  for (const auto& s : all) {
    if (s.scientific == name) return &s;
  }
  return nullptr;
}

void test_birdnet_url_is_the_public_v2_path() {
  SourceConfig config;
  config.source = Source::BirdNet;
  config.detectorUrl = "http://birdnet.local:8080/";  // trailing slash, as a person would type it
  config.limit = 50;
  TEST_ASSERT_EQUAL_STRING("http://birdnet.local:8080/api/v2/detections/recent?limit=50",
                           requestUrl(config, kNow).c_str());
}

void test_inaturalist_url_carries_the_window_and_the_place() {
  SourceConfig config;
  config.source = Source::iNaturalist;
  config.lat = -35.28;
  config.lng = 149.13;
  config.radiusKm = 25;
  config.since = kNow - 30 * 24 * 60 * 60;
  config.limit = 200;
  const std::string url = requestUrl(config, kNow);
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "taxon_id=3"));           // Aves
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "lat=-35.280000"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "lng=149.130000"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "radius=25"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "quality_grade=research"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "per_page=200"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "d1=2026-08-07T00:00:00Z"));  // 30 days before kNow
  config.since = kNow - 90 * 60;  // an hour and a half: the minute survives
  TEST_ASSERT_NOT_NULL(strstr(requestUrl(config, kNow).c_str(), "d1=2026-09-05T22:30:00Z"));
  config.since = 0;
  TEST_ASSERT_NULL(strstr(requestUrl(config, kNow).c_str(), "d1="));
  // v2 is the default and names its fields; v1 asks for nothing and gets it all.
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "api.inaturalist.org/v2/"));
  TEST_ASSERT_NOT_NULL(strstr(url.c_str(), "fields=count,taxon.name,taxon.preferred_common_name,taxon.observations_count"));
  config.inatVersion = 1;
  const std::string v1 = requestUrl(config, kNow);
  TEST_ASSERT_NOT_NULL(strstr(v1.c_str(), "api.inaturalist.org/v1/"));
  TEST_ASSERT_NULL(strstr(v1.c_str(), "fields="));
}

// BirdNET-Go has no window of its own: detections before the window's start
// are dropped from the reply, by their own local date and time.
void test_birdnet_detections_before_the_window_are_dropped() {
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE(parseResponse(Source::BirdNet, kBirdNetBody, seen, &why, "2026-09-06 07:00:00"));
  TEST_ASSERT_EQUAL_size_t(1, seen.size());  // the owl at 06:58 is out; two blackbird calls remain
  TEST_ASSERT_EQUAL_STRING("Turdus merula", seen[0].scientific.c_str());
  TEST_ASSERT_EQUAL_INT(2, seen[0].localCount);
  TEST_ASSERT_TRUE(parseResponse(Source::BirdNet, kBirdNetBody, seen, &why, "2026-09-06 07:10:00"));
  TEST_ASSERT_EQUAL_INT(1, seen[0].localCount);
}

void test_birdnet_repeats_are_counted_not_duplicated() {
  std::vector<Sighting> seen;
  TEST_ASSERT_TRUE(parseResponse(Source::BirdNet, kBirdNetBody, seen));
  TEST_ASSERT_EQUAL_size_t(2, seen.size());
  const Sighting* blackbird = find(seen, "Turdus merula");
  TEST_ASSERT_NOT_NULL(blackbird);
  TEST_ASSERT_EQUAL_INT(2, blackbird->localCount);          // two calls, one species
  TEST_ASSERT_EQUAL_STRING("Eurasian Blackbird", blackbird->common.c_str());
  TEST_ASSERT_EQUAL_INT(0, blackbird->globalCount);         // BirdNET-Go has no such figure
}

void test_inaturalist_keeps_both_counts() {
  std::vector<Sighting> seen;
  TEST_ASSERT_TRUE(parseResponse(Source::iNaturalist, kINatBody, seen));
  TEST_ASSERT_EQUAL_size_t(3, seen.size());
  const Sighting* mallard = find(seen, "Anas platyrhynchos");
  TEST_ASSERT_NOT_NULL(mallard);
  TEST_ASSERT_EQUAL_INT(42, mallard->localCount);
  TEST_ASSERT_EQUAL_INT32(953735, mallard->globalCount);
}

// A real reply, as the board sees it: Canberra, 25 km, 30 days, on
// 2026-09-15. 145 KB, 126 species, every taxon dragging its ancestry and
// photo along. Unfiltered this parse wants 232 KB of heap, which is why the
// frame reported "reply was not the shape expected" on a reply that was.
void test_a_full_size_inaturalist_reply_parses() {
  std::ifstream in("test/test_source/inat-canberra.json");
  TEST_ASSERT_TRUE_MESSAGE(in.good(), "run from firmware/: test/test_source/inat-canberra.json");
  std::stringstream ss;
  ss << in.rdbuf();
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(parseResponse(Source::iNaturalist, ss.str(), seen, &why), why.c_str());
  TEST_ASSERT_EQUAL_size_t(126, seen.size());
  TEST_ASSERT_EQUAL_STRING("Chenonetta jubata", seen[0].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Australian Wood Duck", seen[0].common.c_str());
  TEST_ASSERT_EQUAL_INT(73, seen[0].localCount);
  TEST_ASSERT_EQUAL(37275, seen[0].globalCount);
}

// The same place through v2 with the fields named: a ninth of the bytes and
// the same shape, so the one parser reads both.
void test_a_v2_reply_parses_the_same() {
  std::ifstream in("test/test_source/inat-canberra-v2.json");
  TEST_ASSERT_TRUE_MESSAGE(in.good(), "run from firmware/: test/test_source/inat-canberra-v2.json");
  std::stringstream ss;
  ss << in.rdbuf();
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(parseResponse(Source::iNaturalist, ss.str(), seen, &why), why.c_str());
  TEST_ASSERT_EQUAL_size_t(127, seen.size());
  TEST_ASSERT_EQUAL_STRING("Chenonetta jubata", seen[0].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Australian Wood Duck", seen[0].common.c_str());
  TEST_ASSERT_EQUAL_INT(73, seen[0].localCount);
  TEST_ASSERT_TRUE(seen[0].globalCount > 30000);
}

void test_a_bad_body_says_what_was_wrong() {
  std::vector<Sighting> seen;
  std::string why;
  // A gateway's HTML page: the filter skips what it does not recognise, so
  // this reads as JSON with nothing in it rather than as a syntax error.
  TEST_ASSERT_FALSE(parseResponse(Source::iNaturalist, "<html>gateway timeout</html>", seen, &why));
  TEST_ASSERT_EQUAL_STRING("reply was not the shape expected", why.c_str());
  TEST_ASSERT_FALSE(parseResponse(Source::iNaturalist, "{\"results\":[{\"count\":", seen, &why));
  TEST_ASSERT_EQUAL_STRING("reply was not JSON: IncompleteInput", why.c_str());
  TEST_ASSERT_FALSE(parseResponse(Source::iNaturalist, "", seen, &why));
  TEST_ASSERT_EQUAL_STRING("empty reply", why.c_str());
  TEST_ASSERT_FALSE(parseResponse(Source::BirdNet, "{\"results\":[]}", seen, &why));
  TEST_ASSERT_EQUAL_STRING("reply was not the shape expected", why.c_str());
}

void test_a_bad_body_leaves_the_previous_page_alone() {
  std::vector<Sighting> seen{Sighting{"Turdus merula", "Eurasian Blackbird", 1, 0}};
  TEST_ASSERT_FALSE(parseResponse(Source::iNaturalist, "<html>gateway timeout</html>", seen));
  TEST_ASSERT_EQUAL_size_t(1, seen.size());  // not cleared: the glass keeps what it has
  TEST_ASSERT_FALSE(parseResponse(Source::BirdNet, "{\"results\":[]}", seen));
  TEST_ASSERT_EQUAL_size_t(1, seen.size());  // an object where an array belongs
}

void test_rarest_ranks_on_the_global_count_not_the_local_one() {
  std::vector<Sighting> seen;
  TEST_ASSERT_TRUE(parseResponse(Source::iNaturalist, kINatBody, seen));
  const std::vector<Sighting> page = choose(seen, nullptr, Mode::Rarest, 3);
  // Local counts would put Mallard first with 42. Globally it is the commonest
  // bird on the list by an order of magnitude, so it must come last.
  TEST_ASSERT_EQUAL_STRING("Strix aluco", page[0].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Turdus merula", page[1].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Anas platyrhynchos", page[2].scientific.c_str());
}

void test_most_detected_ranks_on_the_local_count() {
  std::vector<Sighting> seen;
  TEST_ASSERT_TRUE(parseResponse(Source::iNaturalist, kINatBody, seen));
  const std::vector<Sighting> page = choose(seen, nullptr, Mode::MostDetected, 3);
  TEST_ASSERT_EQUAL_STRING("Anas platyrhynchos", page[0].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Strix aluco", page[1].scientific.c_str());
}

void test_artwork_filters_before_ranking_not_after() {
  std::vector<Sighting> seen;
  TEST_ASSERT_TRUE(parseResponse(Source::iNaturalist, kINatBody, seen));
  // Only the owl and the blackbird are drawable. Asking for two birds must
  // return two, not "the two rarest, one of which we then threw away".
  auto drawable = [](const std::string& name) {
    return name == "Strix aluco" || name == "Turdus merula";
  };
  const std::vector<Sighting> page = choose(seen, drawable, Mode::Rarest, 2);
  TEST_ASSERT_EQUAL_size_t(2, page.size());
  TEST_ASSERT_EQUAL_STRING("Strix aluco", page[0].scientific.c_str());
  TEST_ASSERT_EQUAL_STRING("Turdus merula", page[1].scientific.c_str());
}

void test_a_species_with_no_global_figure_sorts_last_in_rarest() {
  // A 0 means the source did not say, not "never recorded anywhere". Sorting it
  // first would make every BirdNET species outrank a genuinely rare bird.
  std::vector<Sighting> seen{
      Sighting{"Strix aluco", "Tawny Owl", 1, 23627},
      Sighting{"Turdus merula", "Eurasian Blackbird", 1, 0},
  };
  const std::vector<Sighting> page = choose(seen, nullptr, Mode::Rarest, 2);
  TEST_ASSERT_EQUAL_STRING("Strix aluco", page[0].scientific.c_str());
}

// The cycle draws what the window has not shown, and only reaches for shown
// birds - oldest first - when it runs out.
void test_cycle_prefers_birds_not_shown_in_the_window() {
  std::vector<Sighting> ranked;
  for (int i = 0; i < 6; ++i) ranked.push_back(Sighting{"bird" + std::to_string(i), "", 10 - i, 0});
  const std::time_t day = 24 * 60 * 60;
  const std::time_t now = kNow;
  // bird0 and bird1 were drawn yesterday, bird2 a fortnight ago, the rest never.
  const auto lastShown = [&](const std::string& n) -> std::time_t {
    if (n == "bird0" || n == "bird1") return now - day;
    if (n == "bird2") return now - 14 * day;
    return 0;
  };
  std::vector<Sighting> page = cycle(ranked, lastShown, now, 7 * 24, 3, 42);
  TEST_ASSERT_EQUAL_size_t(3, page.size());
  for (const Sighting& b : page) {
    TEST_ASSERT_TRUE_MESSAGE(b.scientific != "bird0" && b.scientific != "bird1",
                             "a bird shown yesterday must not come back inside a 7-day window");
  }
  // Same seed, same page: a re-render the same moment does not reshuffle.
  std::vector<Sighting> again = cycle(ranked, lastShown, now, 7 * 24, 3, 42);
  for (size_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_STRING(page[i].scientific.c_str(), again[i].scientific.c_str());
  // Asking for more than the fresh pool holds tops up with the longest-unseen
  // of the shown ones: bird2 before bird0/bird1.
  page = cycle(ranked, lastShown, now, 30 * 24, 5, 7);
  TEST_ASSERT_EQUAL_size_t(5, page.size());
  TEST_ASSERT_EQUAL_STRING("bird2", page[3].scientific.c_str());
  // A window of 0 hours is not the caller's case, but nothing shown ever
  // being "fresh" is: every bird fresh means a plain random `limit`.
  page = cycle(ranked, nullptr, now, 7 * 24, 6, 1);
  TEST_ASSERT_EQUAL_size_t(6, page.size());
}

void test_rarest_is_not_offered_for_birdnet() {
  TEST_ASSERT_TRUE(supports(Source::iNaturalist, Mode::Rarest));
  TEST_ASSERT_TRUE(supports(Source::iNaturalist, Mode::MostDetected));
  TEST_ASSERT_TRUE(supports(Source::BirdNet, Mode::MostDetected));
  TEST_ASSERT_FALSE(supports(Source::BirdNet, Mode::Rarest));
  // eBird has a notable list, which is rarity as a birder judges it; ALA and a
  // plain list carry no global count and get no rarest page.
  TEST_ASSERT_TRUE(supports(Source::eBird, Mode::Rarest));
  TEST_ASSERT_FALSE(supports(Source::Ala, Mode::Rarest));
  TEST_ASSERT_FALSE(supports(Source::JsonList, Mode::Rarest));
}

void test_ebird_url_takes_the_window_as_days_and_the_key_as_a_header() {
  SourceConfig config;
  config.source = Source::eBird;
  config.lat = -35.2809;
  config.lng = 149.13;
  config.radiusKm = 80;                      // over eBird's 50 km cap
  config.since = kNow - 5 * 86400 - 3600;    // five days and a bit: six whole days back
  config.limit = 200;
  config.ebirdKey = "abc123";
  config.ebirdLocale = "en";
  TEST_ASSERT_EQUAL_STRING(
      "https://api.ebird.org/v2/data/obs/geo/recent?lat=-35.280900&lng=149.130000&dist=50"
      "&back=6&maxResults=200&sppLocale=en",
      requestUrl(config, kNow).c_str());
  // Rarest is the notable endpoint, same parameters.
  TEST_ASSERT_EQUAL_STRING(
      "https://api.ebird.org/v2/data/obs/geo/recent/notable?lat=-35.280900&lng=149.130000"
      "&dist=50&back=6&maxResults=200&sppLocale=en",
      requestUrl(config, kNow, Mode::Rarest).c_str());
  // The locale is a query value, escaped like one.
  config.ebirdLocale = "en_AU";
  TEST_ASSERT_TRUE(requestUrl(config, kNow).find("sppLocale=en_AU") != std::string::npos);
  // No window asks for eBird's whole 30 days; the key never lands in the URL.
  config.since = 0;
  TEST_ASSERT_TRUE(requestUrl(config, kNow).find("back=30") != std::string::npos);
  TEST_ASSERT_TRUE(requestUrl(config, kNow).find("abc123") == std::string::npos);
  const auto headers = requestHeaders(config);
  TEST_ASSERT_EQUAL_size_t(1, headers.size());
  TEST_ASSERT_EQUAL_STRING("X-eBirdApiToken", headers[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("abc123", headers[0].second.c_str());
  TEST_ASSERT_EQUAL_size_t(0, requestHeaders(SourceConfig{}).size());
}

void test_ebird_reports_are_one_per_species_with_the_flock_as_the_count() {
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE(parseResponse(Source::eBird, kEBirdBody, seen, &why));
  TEST_ASSERT_EQUAL_size_t(3, seen.size());
  const Sighting* starling = find(seen, "Sturnus vulgaris");
  TEST_ASSERT_NOT_NULL(starling);
  TEST_ASSERT_EQUAL_INT(40, starling->localCount);
  TEST_ASSERT_EQUAL_STRING("European Starling", starling->common.c_str());
  // No howMany means the observer did not count: one, not zero, so it ranks
  // above nothing rather than below everything.
  TEST_ASSERT_EQUAL_INT(1, find(seen, "Strix aluco")->localCount);
  TEST_ASSERT_EQUAL(0, find(seen, "Strix aluco")->globalCount);
  // Recency is the order given, and it survives as the tie-break.
  TEST_ASSERT_EQUAL_STRING("Sturnus vulgaris", seen[0].scientific.c_str());
}

// As api.ebird.org answered for Canberra on 2026-09-18, 20 km and 7 days:
// 153 species, one row each, and 22 notable reports over 9 species.
void test_a_full_size_ebird_reply_parses() {
  std::ifstream in("test/test_source/ebird-canberra.json");
  TEST_ASSERT_TRUE_MESSAGE(in.good(), "run from firmware/: test/test_source/ebird-canberra.json");
  std::stringstream body;
  body << in.rdbuf();
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(parseResponse(Source::eBird, body.str(), seen, &why), why.c_str());
  TEST_ASSERT_EQUAL_size_t(153, seen.size());
  TEST_ASSERT_EQUAL_STRING("Chenonetta jubata", seen[0].scientific.c_str());
  // Clements' English, which is what sppLocale=en gets; en_AU would say
  // Australian Wood Duck, and that is a setting.
  TEST_ASSERT_EQUAL_STRING("Maned Duck", seen[0].common.c_str());
  TEST_ASSERT_EQUAL_INT(10, seen[0].localCount);

  std::ifstream notable("test/test_source/ebird-canberra-notable.json");
  TEST_ASSERT_TRUE(notable.good());
  std::stringstream nbody;
  nbody << notable.rdbuf();
  TEST_ASSERT_TRUE_MESSAGE(parseResponse(Source::eBird, nbody.str(), seen, &why), why.c_str());
  // Reports of one species fold into one sighting; the counts add up.
  TEST_ASSERT_EQUAL_size_t(9, seen.size());
  const Sighting* musk = find(seen, "Biziura lobata");
  TEST_ASSERT_NOT_NULL(musk);
  TEST_ASSERT_TRUE(musk->localCount >= 2);
}

void test_ebird_errors_are_explained_in_its_own_words() {
  // As api.ebird.org answers a distance over its cap, verbatim.
  const std::string bad = R"({"errors":[{"status":"400 BAD_REQUEST","code":"error.data.dist_out_of_range",)"
                          R"("title":"Field dist of rawDataCmd: Distance must be between 0 and 50"}]})";
  TEST_ASSERT_EQUAL_STRING("HTTP 400 - eBird says: Distance must be between 0 and 50",
                           explainStatus(Source::eBird, 400, bad).c_str());
  // A bad key is a 403 with nothing in it.
  TEST_ASSERT_EQUAL_STRING(
      "HTTP 403 - eBird refused the API key; check it, or make one at ebird.org/api/keygen",
      explainStatus(Source::eBird, 403, "").c_str());
  // Every service's own words, in its own shape, as each answered when probed.
  TEST_ASSERT_EQUAL_STRING(
      "HTTP 500 - iNaturalist is having trouble; the next refresh will try again - iNaturalist says: "
      "Elasticsearch error, if this persists please contact the iNaturalist development team.",
      explainStatus(Source::iNaturalist, 500,
                    R"({"error":"Elasticsearch error, if this persists please contact the iNaturalist development team.","status":500})")
          .c_str());
  // iNaturalist's 422 for a wrong path says only "Error", which is not worth repeating.
  TEST_ASSERT_EQUAL_STRING("HTTP 422 - iNaturalist rejected the request; check the place and the radius",
                           explainStatus(Source::iNaturalist, 422, R"({"error":"Error","status":422})").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "HTTP 400 - Atlas of Living Australia says: Query syntax invalid: Error from server at null: Invalid Date String:'nonsense'",
      explainStatus(Source::Ala, 400,
                    "{\n\"message\": \"Error from server at null: Invalid Date String:'nonsense'\", \"errorType\": \"Query syntax invalid\", \"statusCode\": 400\n}")
          .c_str());
  TEST_ASSERT_EQUAL_STRING("HTTP 400 - BirdNET-Go says: limit must be a number",
                           explainStatus(Source::BirdNet, 400, R"({"error":"bad request","message":"limit must be a number","code":400})").c_str());
  // A 404 is HTML, not a message, and the hint is about the address.
  TEST_ASSERT_EQUAL_STRING("HTTP 404 - nothing at that path; check the list URL",
                           explainStatus(Source::JsonList, 404, "<!doctype html><html>...").c_str());
  TEST_ASSERT_EQUAL_STRING("HTTP 404 - nothing at that path; is this a BirdNET-Go instance, and is the port right?",
                           explainStatus(Source::BirdNet, 404, "").c_str());
  TEST_ASSERT_EQUAL_STRING("HTTP 418", explainStatus(Source::iNaturalist, 418, "").c_str());

  SourceConfig config;
  config.source = Source::Ala;
  TEST_ASSERT_TRUE(emptyReplyHint(Source::Ala, config).find("Australia") != std::string::npos);
  config.source = Source::eBird;
  config.radiusKm = 80;
  TEST_ASSERT_TRUE(emptyReplyHint(Source::eBird, config).find("within 50 km") != std::string::npos);
}

void test_ala_url_is_a_species_facet_with_a_solr_window() {
  SourceConfig config;
  config.source = Source::Ala;
  config.lat = -35.2809;
  config.lng = 149.13;
  config.radiusKm = 20;
  config.since = kNow - 7 * 86400;  // 2026-08-30T00:00:00Z
  config.limit = 200;
  TEST_ASSERT_EQUAL_STRING(
      "https://biocache-ws.ala.org.au/ws/occurrences/search?q=%2A%3A%2A&fq=class%3AAves"
      "&fq=occurrence_date%3A%5B2026-08-30T00%3A00%3A00Z%20TO%20%2A%5D"
      "&lat=-35.280900&lon=149.130000&radius=20&facets=species&fsort=count&flimit=200&pageSize=0",
      requestUrl(config, kNow).c_str());
  config.since = 0;
  TEST_ASSERT_TRUE(requestUrl(config, kNow).find("occurrence_date") == std::string::npos);
}

void test_ala_facets_are_species_with_counts_and_the_unsupplied_row_is_skipped() {
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE(parseResponse(Source::Ala, kAlaBody, seen, &why));
  TEST_ASSERT_EQUAL_size_t(3, seen.size());
  TEST_ASSERT_NULL(find(seen, "Not supplied"));
  TEST_ASSERT_EQUAL_INT(78, find(seen, "Chenonetta jubata")->localCount);
  TEST_ASSERT_EQUAL_INT(2, find(seen, "Strix aluco")->localCount);
  TEST_ASSERT_TRUE(find(seen, "Chenonetta jubata")->common.empty());
}

void test_a_json_list_takes_names_or_objects_in_the_order_given() {
  std::vector<Sighting> seen;
  std::string why;
  TEST_ASSERT_TRUE(parseResponse(Source::JsonList, R"(["Turdus merula","Strix aluco","Turdus merula"])",
                                 seen, &why));
  TEST_ASSERT_EQUAL_size_t(2, seen.size());
  TEST_ASSERT_EQUAL_STRING("Turdus merula", seen[0].scientific.c_str());
  TEST_ASSERT_EQUAL_INT(2, seen[0].localCount);  // a repeat counts, as on BirdNET-Go

  // Objects, with BirdNET-Go's own field names accepted, and a wrapper object
  // whose first array is the list.
  TEST_ASSERT_TRUE(parseResponse(
      Source::JsonList,
      R"({"generated":"2026-09-06","birds":[
           {"scientific":"Anas platyrhynchos","common":"Mallard","count":9},
           {"scientificName":"Strix aluco","commonName":"Tawny Owl"}]})",
      seen, &why));
  TEST_ASSERT_EQUAL_size_t(2, seen.size());
  TEST_ASSERT_EQUAL_INT(9, find(seen, "Anas platyrhynchos")->localCount);
  TEST_ASSERT_EQUAL_STRING("Tawny Owl", find(seen, "Strix aluco")->common.c_str());
  TEST_ASSERT_EQUAL_INT(1, find(seen, "Strix aluco")->localCount);

  // Not a list at all: refused, and the previous page kept.
  std::vector<Sighting> kept = seen;
  TEST_ASSERT_FALSE(parseResponse(Source::JsonList, R"({"status":"ok"})", kept, &why));
  TEST_ASSERT_EQUAL_size_t(seen.size(), kept.size());
  TEST_ASSERT_EQUAL_STRING("reply was not a list", why.c_str());
  // The list URL is the request, untouched.
  SourceConfig config;
  config.source = Source::JsonList;
  config.listUrl = "http://homeassistant.local:8123/local/birds.json";
  TEST_ASSERT_EQUAL_STRING(config.listUrl.c_str(), requestUrl(config, kNow).c_str());
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_birdnet_url_is_the_public_v2_path);
  RUN_TEST(test_inaturalist_url_carries_the_window_and_the_place);
  RUN_TEST(test_birdnet_detections_before_the_window_are_dropped);
  RUN_TEST(test_birdnet_repeats_are_counted_not_duplicated);
  RUN_TEST(test_inaturalist_keeps_both_counts);
  RUN_TEST(test_a_full_size_inaturalist_reply_parses);
  RUN_TEST(test_a_v2_reply_parses_the_same);
  RUN_TEST(test_a_bad_body_says_what_was_wrong);
  RUN_TEST(test_a_bad_body_leaves_the_previous_page_alone);
  RUN_TEST(test_rarest_ranks_on_the_global_count_not_the_local_one);
  RUN_TEST(test_most_detected_ranks_on_the_local_count);
  RUN_TEST(test_artwork_filters_before_ranking_not_after);
  RUN_TEST(test_a_species_with_no_global_figure_sorts_last_in_rarest);
  RUN_TEST(test_cycle_prefers_birds_not_shown_in_the_window);
  RUN_TEST(test_rarest_is_not_offered_for_birdnet);
  RUN_TEST(test_ebird_url_takes_the_window_as_days_and_the_key_as_a_header);
  RUN_TEST(test_ebird_reports_are_one_per_species_with_the_flock_as_the_count);
  RUN_TEST(test_a_full_size_ebird_reply_parses);
  RUN_TEST(test_ebird_errors_are_explained_in_its_own_words);
  RUN_TEST(test_ala_url_is_a_species_facet_with_a_solr_window);
  RUN_TEST(test_ala_facets_are_species_with_counts_and_the_unsupplied_row_is_skipped);
  RUN_TEST(test_a_json_list_takes_names_or_objects_in_the_order_given);
  return UNITY_END();
}
