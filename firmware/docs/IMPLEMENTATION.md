# Implementation reference

> **Only tested on hardware with the XIAO ESP32-S3 Plus in the EE02, with the
> Australian plates.** The reTerminal E1004 pin map and SD card handling, and
> the European and North American packs, compile and render on the desktop
> harness but have not been run on a board.

Constants, pin numbers and algorithms needed to write the C++ port, gathered so
they don't have to be re-derived. Everything here was read out of this repo or
verified against live hardware documentation on 2026-09-04.

Read `README.md` first for the decisions; this file is the detail under them.

---

## 1. Boards

Two boards carry the same 13.3" Spectra 6 glass and the same T133A01 controller
pair. `firmware/lib/panel/panel.h` holds both pin maps behind `BOARD_E1004`;
`platformio.ini` has an environment for each.

| Signal | EE02 (XIAO ESP32-S3 Plus) | reTerminal E1004 |
| --- | --- | --- |
| Panel CLK / MOSI | 7 / 9 | 7 / 9 |
| Panel CS master / slave | 44 / 41 | 10 / 2 |
| Panel DC | 10 | 11 |
| Panel BUSY | 4 | 13 |
| Panel RESET | 38 | 38 |
| Panel power enable | 43 | 12 |
| User keys 1 / 2 / 3 | 2 / 3 / 5 | 3 / 4 / 5 (KEY0-2, front panel) |
| Battery sense | none | ADC on 1, enable on 21, 1:2 divider |
| Flash | 16 MB, one regional pack | 32 MB, one regional pack at ~690-900 px |
| SD slot | none | CS 14, MISO 8, detect 15 (LOW = card), power 16 (HIGH = on); CLK and MOSI shared with the panel |
| Also on board | - | PCF8563 RTC, SHT4x on I2C 19/20, buzzer 45, LED 48 |

The E1004's numbers are from Seeed's `GxEPD2_reTerminal_E1004` example, the
reTerminal E-series peripherals cookbook (`MicRecordToSD.ino`) and the ESPHome
configuration, which states outright that "SD and ePaper share the same SPI
bus"; they have not yet been proved on a board here.

**One bus, two chip selects.** The panel is write-only, so the card's MISO is
the only pin it adds. Both sides go through the one `SPIClass` (`sharedSpi()`
in `panel.cpp`, begun on first use with MISO 8 on the E1004 and none on the
EE02), each selecting its chip inside its own SPI transaction, and the panel's
two selects are driven high before anything else touches the bus. Seeed's own
sketch instead opens the card on a second `SPIClass(HSPI)` over the same pins,
which only works because the last `begin` wins the GPIO matrix; one instance
has no such race. The card is powered on wake and off before deep sleep (a
powered card idles at a milliamp or two); the flow is read the pack, render,
then push to the glass, so the two never contend within a page anyway.

**Every `--fit` bake starts from 1200 px** and shrinks to its budget (a
sample estimate, then the real bake confirmed and shrunk again if over), so
a pack fills the room it has rather than stopping at a fixed 448: the XIAO's
EU pack had sat at 10.2 of 13.4 MB. The E1004's one region gets all 30 MB,
690-900 px (`firmware/packs/e1004/`), chosen on the flasher page as on the
XIAO. The firmware still reads several `/plates-<region>.bin` if it finds
them - a frame flashed with the old every-region image, or the packs on an
SD card - and the settings page then grows an artwork picker.

**The card holds the full-size pack.** `firmware/packs/card/<region>.bin` is
baked at 1200 px (`--source 1200`, no `--fit`) - the size the artwork ships
at, about 75 MB a region - and is offered on the flasher page as a download,
not flashed: the owner copies it to the root of a FAT32 card as
`plates-<region>.bin`, the same name the flash filesystem uses, and
`App::openPack()` prefers the card's copy of a region over the flash's. A
card-less E1004 draws from flash as before. Drawing a 1200 px sprite at page
size shrinks it two to three times, so `drawSprite` box-filters when the
ratio reaches 2x (nearest-neighbour below that, as ever): sampling one source
pixel in nine would throw the card's resolution straight back away. Peak
PSRAM with a 1200 px sprite open beside the 1200 x 1600 canvas is about
6.3 MB of the 8.

The pack's posterisation - fifteen greys a pixel, sixteen chroma values a
bird, one a 4x4 block - was weighed against the plates whole (32-bit BMPs, or
PNGs through a streaming decoder) on the host harness. After the six-ink
dither the two are indistinguishable; before it the pack shows its chroma
blocks as a 4-pixel grid across colour edges. So the pack stays, and the
grid is taken out where the sprite is drawn: `SpriteImage::at` interpolates
the chroma bilinearly between block centres (fixed point, a pixel's position
among the centres being (x - 1.5) / 4), after `loadSprite` has given every
unpainted block its painted neighbours' mean so the blend at the outline
leans on the bird. `bake_plates.reconstruct` does the same, so what the
tools show is what the frame draws. Nothing in the file changed.
The bake later took the blend into account too: the block chroma is fitted by
least squares through it, the 16 pairs clustered from those fitted values as
distinct colours (weighted by the square root of their count, so a large dull
field cannot take half the table), and the pairs fitted again once the codes
are fixed. Against the block means that is 25% less error in colourful
pixels, and fewer chroma bytes; still no change to the file.

v7 did change the file. Both fits now weight a pixel by its brightness, so
the black between a rosella's yellow scallop edges stops dragging the block's
chroma to grey. zlib gave way to a context-model range coder
(`planecoder`): each code is coded with odds picked by its decoded neighbours
- up, left and the slope of the row above for luma, from a prior learned over
278 plates and compiled in; up, left and the block's luma band for chroma,
from flat, and only for blocks with something painted in them. Against zlib
that is 65% of the luma bytes and 76% of the chroma. The saving went on 2x2
chroma blocks - thin colour between dark marks, which 4x4 averaged away,
kept at 86-99% where it had been 71-97% - and on resolution, since `--fit`
bakes as large as the budget allows. tinf went with zlib. The decoder is
about 2.7x inflate's time a pixel on the host.

### The EE02 pin map - not documented by Seeed

This is the single most expensive thing to rediscover. Seeed's wiki gives no pin
table for the EE02. These come from the
[ESPHome component](https://github.com/rkaramandi/esphome-seeed-ee02) driving
the same board, and should be confirmed against the schematic PDF before a
board is powered up.

| Signal | GPIO | Notes |
| --- | --- | --- |
| Panel CLK | 7 | shared SPI bus (XIAO D8) |
| Panel MOSI | 9 | shared SPI bus (XIAO D10) |
| Panel CS master | 44 | |
| Panel CS slave | 41 | dual-CS, unusual — both needed |
| Panel DC | 10 | |
| Panel BUSY | 4 | |
| Panel RESET | 38 | |
| Panel power enable | 43 | UART0 TX on a bare XIAO: Serial must be the USB CDC port |
| User keys 1 / 2 / 3 | 2 / 3 / 5 | active low; the ESPHome port reads them off the schematic. All RTC-capable, so all three wake the frame |
| *(free)* | 8 | XIAO D9 |
| *(free)* | 1 | XIAO D0 |

`firmware/lib/panel/panel.h` carries the same numbers. The panel's register
sequence there is Seeed_GFX2's `Driver_T133A01` (the vendor's own, verified on
this board upstream), and the framing - two controllers, 600 columns each,
`[master 300 bytes][slave 300 bytes]` per row, BUSY low while busy - agrees
with the ESPHome port. Seeed's SPI clock is 10 MHz; ESPHome found 2 MHz its
ceiling. Neither has been measured on this bench.

The panel is **write-only**: it never drives MISO, so GPIO8 is free for the card.

**Button wake:** ESP32-S3 supports `ext1` only (no `ext0`), and only GPIO 0–21
are RTC-capable. Seeed does not publish which pins the three user buttons sit
on; if they fall outside 0–21 they cannot wake the board from deep sleep. Meter
this before designing a long-press to open the settings portal.

```c
esp_sleep_enable_ext1_wakeup(BIT(pin), ESP_EXT1_WAKEUP_ANY_LOW);
// on wake:
esp_sleep_get_wakeup_cause();  // TIMER -> render; EXT1 -> settings portal
```

Arming ext1 costs ~5 µA on top of the 14 µA baseline.

**Power and charging.** The EE02 has a JST 2.0 mm battery input, a power
switch and its own charger (TI BQ2407x), so charging is the board's, not the
XIAO's. The board straps the charge current to **about 200 mA** and the
charger's safety timer to **about 8 hours**, so it is built for a cell of
**roughly 2000 mAh**: that fills in a night, and a larger pack stops charging
when the timer runs out, part-full. The charger powers the frame from USB
directly while it charges (a frame on mains does not cycle its cell), and its
`CHG` and `PGOOD` outputs say whether it is charging and whether power is
present; the battery voltage itself has to be read through a divider on an
ADC. The switch is in series with the battery, so a frame left switched off
charges nothing.

Seeed does now document the three user buttons functionally - refresh, previous
page, next page - but still gives no GPIOs for them, so the RTC-capability
question above stands.

---

## 2. Packer constants

From the packer (`firmware/lib/packer/packer.h`). These are not tunables — several were
chosen to fix specific visual bugs, noted where known.

```
_PACK_SHORT    = 1200   # packing always happens at this short-side size,
                        # then placements are SCALED to the output.
_STEP          = 6      # spiral radius increment, and arc spacing
_PROBE_BANDS   = 3      # cheap pre-test rows per sprite
_ALPHA_CUTOFF  = 24     # alpha > this counts as opaque
_OVERLAP_PX    = 2      # collision mask eroded by this (MinFilter 5x5)
_MARGIN        = 0.04   # page edge to content, short side
_MAX_BIRDS     = 40     # cap for render speed
```

**Pack at `_PACK_SHORT`, not at output size.** The packer works in whole pixels
and is *not* scale-invariant. Packing at the output resolution instead made the
panel and a preview at another resolution place birds differently. Scale placements afterwards:
`scale = min(resolution) / _PACK_SHORT`.

Sprites and labels are redrawn from source at the target size — never resampled
from the packed raster.

### The algorithm

1. Sort sprites largest-first (see §3 for what sets size).
2. Build each collision mask: threshold alpha at `_ALPHA_CUTOFF`, then **erode**
   by `_OVERLAP_PX` (a 5×5 minimum filter) so birds nestle into each other's
   invisible paper halos while their bodies never overlap.
3. For each sprite, walk a spiral of candidate positions out from the canvas
   centre: `count = max(8, int(2*pi*r / _STEP))` positions at each radius,
   `r += _STEP`, up to `hypot(width, height)`.
4. At each candidate: reject if it would leave the canvas; then test the three
   banded probe rows first (a row costs ~1/100th of the full box and most
   candidates collide); only then test the whole footprint.
5. First non-colliding position wins. OR the mask into the occupancy grid.
6. If any sprite cannot be placed, the whole pack fails — caller retries smaller.
7. Finally, shift the packed cluster so its bounding box is centred.

A probe row is one row of the sprite mask, chosen as the densest row within each
of three horizontal bands — banded rather than simply the three densest rows,
which would all land in the body and catch the same collisions.

### Porting notes

`numpy` boolean arrays become **bitsets** — one bit per pixel, not one byte.
That is where the ~1000× speedup lives: a 1200×1200 occupancy grid is 176 KB
instead of 1.4 MB, and collision tests become word-wise `AND` with early exit.
Measured/estimated: ~3.5 h interpreted → ~0.26 s native (3.9 s absolute worst
case with no early exit).

Labels join their bird's collision mask, so a name tucks under a body and never
lands on a neighbour. A label is *centred in the box reserved for it*, because a
re-rasterised font is not exactly `width × scale`.

The C++ takes three liberties with the *cost* of the above, none of which move a
bird — `firmware/test/test_packer` holds each against the loop it replaced:

- The erosion, the rescale and the label's centroid scan are word-wise rather
  than per-pixel. The erosion is separable and, on a bitset, a shift-and-AND on
  both axes.
- Step 3's sweep stops before `hypot(width, height)`. A candidate is on the page
  only while the ellipse clears the sprite's own margin — `|cos a| < limX/(r·ax)`
  and `|sin a| < limY/(r·ay)` — and once `limX/(r·ax)` and `limY/(r·ay)` are both
  under 1 with their squares summing to ≤ 1, no angle satisfies both and the walk
  is over. Worth having because the scale search opens with a set too big for the
  page on purpose, so the wasted laps were walked several times per layout.
- Step 4's full footprint check reads rows from the outside in, skipping the
  empty ones and the probe rows already known clear. Collisions are at the edges,
  because the spiral walks a bird outward until it stops overlapping.

---

## 3. Bird size

Every bird is drawn at the same size. The packer finds one scale for the whole
set - starting too big for the page and shrinking to the first size that fits,
so the collage fills the paper - and hands every silhouette that scale. The
first bird in the list is placed first and takes the middle of the page, so
the source's ranking is the order of precedence. There is no body-mass table,
no ink-density correction, and nothing about a bird's size in the pack.

### Layout variants

The same set of birds must not always produce the same page. A `layout` integer
changes two things, and neither costs packing quality:

| | |
| --- | --- |
| `turnFor(layout)` | where on the ellipse the sweep starts. The spiral tries candidates in angle order, so the first bird placed at each radius lands wherever the sweep begins; moving that start rearranges the cluster while every candidate is still tried. Stepped by the **golden angle** rather than `2π/n`, so successive layouts land far apart instead of cycling through a handful of arrangements |
| `flipFor(baked, name, layout)` | whether a bird is mirrored. Still decided per bird from its own name, so an arriving bird cannot re-roll its neighbours |

**The layout must never be what triggers a render.** On the Pi the trigger key is
computed with the layout pinned, and the counter advances only once a render is
already due — so the refresh budget is unchanged and a page is still a function
of its species set. The ESP32 wants the same discipline for the same reason: an
e-ink refresh is the expensive thing, not the pack. It advances the layout at
the start of every page it draws, so the timer - never the layout - decides
when there is a render, and each render is a new arrangement. That also means
the skip of a page identical to the one on the glass no longer fires: the
layout is part of the page's signature.

---

## 4. Colour and paper

`firmware/tools/artwork.py` (`dither`) — the palette must map 1:1 onto the driver's
or the remap lands on the wrong colour. `tests/test_panel.py` pins this.

Driver index order is **black, white, yellow, red, blue, green**:

```
desaturated  (0,0,0) (255,255,255) (255,255,0) (255,0,0) (0,0,255) (0,255,0)
saturated    (0,0,0) (161,164,165) (208,190,71) (156,72,75) (61,59,94) (58,91,70)
PALETTE_6 = desaturated*(1-0.5) + saturated*0.5      # SATURATION = 0.5
```

Dither against the blend — it measures colour distance in roughly panel space.

Paper base tone is `TARGET_PAPER = (242, 237, 226)`. On the panel the collage is
rendered **flat, not textured**: the grain would dither into noise. `PAD = 16`
is the transparent margin each sprite carries for its halo feather to bleed into.

**Labels are hard-thresholded to pure black on the panel** — antialiased grey
dithers into colour speckle.

The plates carry no words beyond the bird's name.

---

## 5. Battery

**What exists:** on the E1004 the status page shows the cell voltage, read
through the board's 1:2 divider on GPIO1 with the sense enable (GPIO21) held
high only for the reading, so the divider does not drain the cell in sleep
(`App::statusLines`). The EE02 brings no battery sense out, so its status
page shows nothing. The bird page carries no indicator and there is no
setting for one.

**If an indicator is added to the bird page**, it has to be reserved in the
packer's occupancy grid *before* the first bird is placed and excluded from
the cluster's final centring - the same trick the labels use - never painted
on afterwards, because the packer fills the page from the centre out and
anything drawn later lands under a bird. The charger's `CHG` and `PGOOD`
outputs (§1) would let it say *charging* rather than infer it from a rising
voltage, if either reaches a GPIO.

## 6. Where the birds come from

**Five sources, a user setting** (`firmware/lib/source/`). They answer different
questions and none is strictly better:

| | BirdNET-Go | iNaturalist | eBird | ALA | JSON list |
| --- | --- | --- | --- | --- | --- |
| what it knows | what a mic heard at this address | what people recorded nearby | what birders reported nearby, 30 days | every record near here, in Australia | whatever the owner's script says |
| needs | an instance on the LAN | a latitude and longitude | a place and a free key | a place | a URL |
| local count | detections, counted per species | `count`, already aggregated | `howMany` of the latest report | facet `count` | `count`, or 1 |
| global count | **none** | `taxon.observations_count` | none, but a *notable* list | none | `global`, if given |
| rarest mode | not available | yes | yes: eBird's notable list | not available | not available |

All public HTTP; only eBird wants a key, and it is free and instant from
`ebird.org/api/keygen`. It travels as a header, never in the URL, so it is in
no log line.

The URL building, the parsing and the ranking are pure functions over strings, so
`pio test -e native` covers them against canned bodies with no radio involved -
the same reason `lib/packer` is Arduino-free. Only the fetch needs WiFi.

### BirdNET-Go

```
GET {detector_url}/api/v2/detections/recent?limit=200
```

Public, no auth (it strips the recording `source` field for unauthenticated
callers, which the frame does not want anyway). Answers with a **bare array**,
not an envelope. Fields used: `scientificName` (the artwork key), `commonName`
(label text). Also present: `date`, `time`, `confidence`, `speciesCode`.

One row is one call of one bird, so a species arrives many times and the repeats
*are* its local count.

**The API is v2 whatever BirdNET model the instance runs.** The API version and
the model version are unrelated and there is no v3 of either path - v2.4 versus
v3.0 is a question about the *label vocabulary*, not the endpoint.

### iNaturalist

No API key. Set a descriptive User-Agent. Rate limit is generous (~60/min); an
hourly device is nowhere near it. Bulk *Commons* metadata scraping does 429 -
throttle that if the artwork pipeline gets rebuilt.

```
GET https://api.inaturalist.org/v1/observations/species_counts
    ?taxon_id=3           # Aves
    &lat=..&lng=..&radius=..          # km
    &d1=YYYY-MM-DD                    # lookback window start
    &quality_grade=research
    &per_page=200
```

Returns every species in the radius, already sorted by local count. Each result:

```
count                              -> local sightings, for "most detected"
taxon.name                         -> scientific name; the artwork key
taxon.preferred_common_name        -> label text
taxon.observations_count           -> GLOBAL total, for "rarest"
```

**Both modes come from this one call.** Most-detected sorts on `count`
descending; rarest sorts on `taxon.observations_count` *ascending*.

iNaturalist has **no confidence score** (it has `quality_grade`:
research / needs_id). BirdNET-Go's `confidence` has no equivalent here.

### eBird

```
GET https://api.ebird.org/v2/data/obs/geo/recent?lat=..&lng=..&dist=..&back=..&maxResults=200&sppLocale=en
GET https://api.ebird.org/v2/data/obs/geo/recent/notable?...       # the rarest page
X-eBirdApiToken: <key>
```

`dist` is capped at 50 km and `back` at 30 days by eBird itself; the frame
clamps its radius and turns the lookback instant into whole days back. Answers
with a **bare array** of reports, **one per species** - its most recent - so
"most seen" here means "reported most recently, flock size as the tie-break",
which is honest for what a reporting network knows. Fields used: `sciName`,
`comName`, `howMany` (absent when the observer did not count; taken as 1). A
wrong key is a bare 403, which the frame names as such.

The notable endpoint answers the same shape, one row per report rather than
per species, of sightings eBird's regional filters flag as unusual for the
place and the date. That is rarity as a birder judges it rather than as a
global count says it, and it is what eBird's Rarest draws.

eBird's names follow the Clements checklist, which is a third opinion on
binomials beside BirdNET's and iNaturalist's - the alias rows in the plate
index (§7) are what absorb that.

### Atlas of Living Australia

```
GET https://biocache-ws.ala.org.au/ws/occurrences/search
    ?q=*:*&fq=class:Aves
    &fq=occurrence_date:[<ISO> TO *]        # the lookback window, Solr syntax
    &lat=..&lon=..&radius=..                # km
    &facets=species&fsort=count&flimit=200&pageSize=0
```

No key. A **facet over species and no records**: `pageSize=0` keeps the reply
to the counts, 26 KB for Canberra's 200 species where the records themselves
would be megabytes. ALA ingests eBird Australia, BirdLife's atlas and
iNaturalist, so for an Australian frame it is the fullest local list there
is - the first Canberra probe returned 1,546 records in six weeks within 20 km,
led by Wood Duck, Superb Fairywren and Australian Magpie. Records with no
species land in a bucket labelled "Not supplied" whose `fq` is a negation; the
parser skips on the `fq`, and on any label that is not a binomial. No common
names come with a facet; the page falls back to its own label file.

### A JSON list

```
GET <the URL as given>
```

Whatever the owner serves. A bare array of scientific names, or an array of
objects carrying `scientific` (BirdNET-Go's `scientificName` is accepted too,
so a detector's own export drops straight in) and optionally `common`, `count`
and `global`; an object at the top is searched for its first array, so
`{"birds": [...]}` works without being the documented shape. The order given
is the ranking when there are no counts - a list is already someone's choice.
No window: the list is what the list is. This is the escape hatch for every
source without a client: a Home Assistant automation, a Bird Buddy export, a
cron job beside a BirdNET-Pi.

### Rules that hold for all of them

**Rarest must use the global count.** Ranking by fewest local sightings looks
like rarity but is not: in Bergen it puts Mallard (953,735 observations
worldwide) top, purely because one person logged it once, over Tawny Owl
(23,627). Global counts are also effectively unique - zero ties across all 179
Brisbane species - so there is no tiebreak rule to write and the page does not
reshuffle between refreshes.

Which is why **rarest is not offered on BirdNET-Go, ALA or a list**
(`supports()`), rather than silently ranked on something else. A frame set to it should say the source cannot
answer, not draw a page that quietly means a different thing. For the same reason
a species whose global count is 0 sorts *last*: a 0 means "this source did not
say", not "never recorded anywhere".

**Filter by available artwork BEFORE ranking, not after.** Species with no
illustration are dropped from the collage; ranking first and filtering second
makes the rare mode pick six birds and silently discard most of them.

**The two disagree on binomials.** iNaturalist says `Icthyophaga leucogaster`
after a 2023 split where BirdNET still says `Haliaeetus leucogaster`. The artwork
lookup has to tolerate either spelling - which is what the union of the shipped
label sets is for (§7).

**A failure says what the service said.** `explainStatus()` turns a non-200
into one line in the service's own words where it gave any - eBird's
`{"errors":[{"title":...}]}` ("Distance must be between 0 and 50"), iNaturalist's
`{"error":...}`, ALA's `{"message":...,"errorType":...}`, BirdNET-Go's
`{"message":...}` - with the usual meaning of a 403, 404 or 429 for that source
beside it; a 404 is HTML and gets a hint about the address instead. An empty
reply gets `emptyReplyHint()`: the radius and the window to widen, and for ALA
the reminder that it covers Australia alone. Below HTTP, `fetch()` names the
host and what a refused connection or a timeout usually means at it, and
resolves `.local` names by mDNS first, since the ESP32's resolver does not and
"nothing on this network calls itself birdnet-go.local" is the honest failure.
All of it reaches the settings page's **Test source** and the status page.

**A malformed reply leaves the previous page alone.** `parseResponse` returns
false and does not touch the caller's vector: a half-parsed page is worse than
the one already on the glass, and an e-ink refresh is expensive.

---

## 7. Artwork name → file

`artwork.normalize()` in `firmware/tools/`. `"Turdus merula"` → `turdus-merula.png`, plus curated
variants `turdus-merula-2.png`, `-3`, … Lowercase, non-alphanumerics to hyphens.

No runtime alias map — assets were renamed to modern eBird / BirdNET names in a
one-off migration. `tests/test_artwork_names.py` enforces that every filename is
a name *something* recognises: a label from any shipped set
(`assets/birdnet_labels_v*.txt`, v2.4 and the v3.0 preview taken as a **union**,
since v3 adds 3563 species and drops 288), a style's own `species-extra.txt`, a
hybrid (`-x-`), or a listed exception.

**There is no BirdNET requirement on artwork.** Images are keyed on the
scientific name, so a plate no classifier can name is still reachable from
iNaturalist — 335 of the 871 bird species recorded in Australia have no v2.4
label at all. The check exists to catch typos, because the lookup is an exact
match and a misspelt file is unreachable forever without ever complaining.

The variant choice is **per species, not per render**, and is persisted so a
restart doesn't reshuffle the page.

Artwork lives in style folders: `assets/artwork/<style>/birds/`, each with its
own `ATTRIBUTION.md` and `manifest.json`. One style active at a time; no union
across styles. An empty page shows the status page.

Current state: `au` = 770 files / 723 species (Gould's *The Birds of
Australia* plus one plate apiece for the birds he lacked), `eu` = 806 files /
415 species (Gould's *Birds of Europe*, von Wright's *Svenska Fåglar*), `us`
in progress (Audubon).

---

## 8. Suggested build order

Each step is verifiable on its own; do not skip to 4.

1. **Bake tool** — done, `firmware/tools/bake_plates.py`. One pack per style:
   per species a 1-bit silhouette, a 15-colour posterised sprite (4-bit, with a
   map into a 256-colour palette shared by the pack), mass, label box and flip.
   *Not* pre-dithered: a dither is only right at the size it is drawn at, and
   the packer decides that size at render time. Format in the docstring; a
   Python decode of one sprite back to PNG was the check.
2. **Host-side packer harness** — done, `src/native`, and it now renders the
   whole page (`lib/render`: composite, Floyd–Steinberg to the six inks,
   TrueType labels) plus the setup and status pages to a PPM.
3. **Panel push** — written, `lib/panel`, not yet run. A thin driver rather
   than Seeed_GFX2 itself: init, stream, refresh, sleep, with the vendor's
   register values. `Actions → Test pattern` in the web UI is the first thing
   to put on the glass.
4. **Data source, WiFi, portal, sleep loop** — written, `src/device`, not yet
   run. HTTPS to iNaturalist is `setInsecure()`: the frame has no clock at
   first boot and no root store; a CA bundle is a later refinement. Settings
   live in NVS (`Preferences`, namespace `frame`), the frame's own memory in
   `state`. Keys: 1 turns the portal on, 2 the status page, 3 a new page.
   Without a network, or after three failed joins, the frame is its own AP
   with a captive portal and a WiFi QR code on the glass.

What is not there: a battery indicator on the bird page (§5 - the E1004's
status page shows the voltage, the EE02 has no sense pin); OTA (single app
slot, on purpose - the plates want the flash). An empty page shows the status
page.

Fidelity target is "matches the Python closely" — the silhouette packer,
feathered halos, labels and dithering all port. `packing-example.png` is the
reference render; `settings-ui.html` was the settings page the firmware's
(`src/device/webui.cpp`, one template, no scripts) stands in for.

**Memory, as built.** Everything big is `std::vector`, which the Arduino core
sends to PSRAM above 16 KB: canvas 1.9 MB, frame 1.9 MB, font 0.9 MB, pack
index ~45 KB, occupancy grid 176 KB, a page's masks and sprites a few hundred
KB. Internal SRAM is 15% used at boot and the WiFi/TLS stack takes its share
from there.

**Flash, as built.** App 1.27 MB of the 2 MB `factory` slot; the `plates`
LittleFS partition is 14.3 MB and holds the pack plus the font. Gould's 548
species fit at 448 px on the long side (11.7 MB); 480 px does not.

---

## 9. Known-unknown list

Things deliberately unresolved, so nobody assumes they were settled:

- Panel refresh duration (~30 s assumed) — the whole battery table depends on it.
- **Whether the charger's `CE` or an `EN` pin reaches a GPIO** (§1). The ~8 h
  safety timer caps the usable cell at ~2000 mAh; a frame that could reset the
  timer itself could charge a larger pack in stages.
- **The pin budget for battery sense on the EE02** (§1, §5). GPIO1/2 are the
  only free pins. Battery voltage needs an ADC, and `CHG`/`PGOOD` want two
  more - so either they share one ADC through a resistor ladder, or something
  gives. Resolve this before the board is wired, not after. (The E1004 has
  its battery sense on GPIO1 already.)
- Whether the user buttons really are GPIO 2/3/5 (§1) - taken from the ESPHome
  port, not metered here. They are RTC-capable if so, and the firmware arms all
  three for wake with internal pull-ups.
- Whether 10 MHz SPI to the panel is clean on the EE02's flex - Seeed's
  figure; the ESPHome port needed 2 MHz. `kSpiHz` in `lib/panel/panel.cpp`.
- Whether the Spectra 6 refresh fits the 90 s BUSY timeout in every
  temperature the frame will hang in. Cold panels are slower.
- **Artwork coverage outside northern Europe.** Brisbane is 1/30 species. 815
  public-domain Gould *Birds of Australia* plates exist on Wikimedia Commons,
  but the species is in each file's *metadata*, not its filename, and
  `scripts/` — the entire curation pipeline — is gitignored and absent from
  this clone. It has to be rebuilt before a style can be added.
