# firmware

The frame's C++ (see [`docs/`](docs/) for the pin
map, the packer constants and the build order).

## Building

```bash
pio run -e native                                   # host build, no hardware
pio test -e native                                  # host tests over lib/ (packer, source, render)
pio run -e xiao                                     # XIAO ESP32-S3 Plus in the EE02
python3 firmware/tools/webflash.py                  # build every board and region, flash from Chrome/Edge
python3 firmware/tools/webflash.py --boards ee02 --regions au   # just the one image
```

**Flash through the web flasher.** It is what decides what goes on a board:
the app, and a plates image per region built from `firmware/packs/<board>/`
with the fonts subset afresh from `assets/`, each staged apart from the rest
so nothing stray ends up in it. The page has a toggle for the region and
separate buttons for a first install, the app alone, and the plates alone
(`--bake` bakes any pack that is missing). It never touches `firmware/data/`.

By hand, for development, `uploadfs` flashes whatever is in `firmware/data/` -
which is the host harness's working set and is **not** an EE02 image as it
stands (its `plates.bin` is the E1004's 30 MB pack). Put a board's pack there
first:

```bash
pio run -e xiao -t upload                           # flash the app over USB
cp firmware/packs/ee02/au.bin firmware/data/plates.bin
.venv-rembg/bin/python firmware/tools/subset_font.py assets/fonts/gentiumbookplus/GentiumBookPlus-Italic.ttf firmware/data/label.ttf
.venv-rembg/bin/python firmware/tools/subset_font.py --caps assets/fonts/gould/GouldCondensed-Regular.ttf firmware/data/name.ttf   # the common name
echo au > firmware/data/region.txt                  # which region, for the web plates
pio run -e xiao -t uploadfs                         # flash the plates + font image
```

**Full-size plates from the web.** A supplement to the plates in flash, not a
replacement: when a bird is drawn more than 1.25x its flash plate's size - a
page of one or two birds - the frame fetches the same plate at full size from
the site in its settings (`<url>/<Scientific_name>.bin`; a `{region}` in the
URL becomes the plates' region, for a site with a folder a region) and draws
the flash plate whenever that fails. After one failure the rest of the page
stays in flash. The files are split from the SD-card packs:

```bash
npx wrangler login                                  # once
python3 firmware/tools/export_web_plates.py --deploy fragrant-heart-ad53
```

The plates go up under a folder named by a random key (made on the first run,
kept in the git-ignored `firmware/webplates.key`), all three regions, ~240 MB in
1,702 files - uploaded as the static assets of the Worker behind
birdpngs.c4k3.xyz (`fragrant-heart-ad53`), with Wrangler rather than the
dashboard, whose drag-and-drop stops at 1,000 files. The script prints the frame's URL,
`https://<host>/<key>/{region}`; it goes in the frame's web UI and nowhere
else. `--new-key` makes a fresh key and retires the old address with the next
deploy.

The public copy needs none of that: the flasher's own build (`webflash.py`)
splits the card packs into `plates/<region>/` beside the page, so the GitHub
Pages site the workflow publishes carries them, and the firmware's default
address is https://c4kew4lk.github.io/bird_poster/plates/{region}. The
Cloudflare copy is for a private address entered in the web UI instead.

The host harness renders the real page from a plate pack and writes a PPM:

```bash
.pio/build/native/program firmware/packs/ee02/au.bin page.ppm --layout 3 \
    --names "Dacelo novaeguineae,Cacatua galerita,Malurus cyaneus"        # the real page
.pio/build/native/program --setup setup.ppm                              # the AP page
.pio/build/native/program --status status.ppm                            # the diagnostics page
```

The page's settings have flags of the same names: `--pack classic|grid|scatter|hero`,
`--portrait`, `--count N`, `--vivid/--sharpen/--edges N`, `--cream N` (paper
tone, 0-4), `--jitter N` (dither randomisation), `--resample
bilinear|mitchell|catmullrom` (how an enlarged plate is filled in),
`--sci-percent N` (scientific name size), `--margin PX` (or `--margins TOP
RIGHT BOTTOM LEFT`), `--date STYLE` with `--date-us` and
`--date-pos t|b l|c|r`, and `--note TEXT` (the refresh counter's line). For
looking into the pipeline: `--canvas FILE.ppm` writes the page as the dither
receives it, and `--web-sprite FILE.bin` draws the first bird from a
full-size web plate through the frame's own decoder.

PlatformIO is a workstation tool the frame never needs, so it lives in its own
gitignored venv rather than in the project's dependencies - the same arrangement
`.venv-rembg` has:

```bash
python3 -m venv .venv-pio && .venv-pio/bin/pip install platformio
.venv-pio/bin/pio run -e xiao
```

Current cost on the XIAO ESP32-S3: **RAM 15%** of internal SRAM at boot,
**flash 1.27 MB** of the 2 MB app partition. The page lives in PSRAM: a
1600x1200 canvas and frame are 1.9 MB each, the font 0.9 MB, and a page's
sprites a few hundred KB more.

## Layout

| Path | What |
| --- | --- |
| `lib/packer/` | the silhouette packer. Arduino-free, so both environments share it |
| `lib/source/` | where the birds come from: BirdNET-Go, iNaturalist, eBird, the Atlas of Living Australia or a JSON list, a user setting. Also Arduino-free |
| `lib/plates/` | reader for the plate pack: index in RAM, sprites and silhouettes decoded on demand by the range coder (`planecoder.cpp`) |
| `lib/render/` | canvas, sprite blit, Floyd-Steinberg to the six inks, TrueType labels, QR codes, and the three pages (birds, setup, status). Arduino-free |
| `lib/panel/` | the T133A01 driver: init, stream a frame to the two controllers, refresh, sleep. Arduino |
| `lib/stb_truetype`, `lib/qrcodegen` | vendored: TrueType rasteriser, QR encoder. Licences alongside |
| `test/` | host tests over `lib/`, run with `pio test -e native` |
| `src/native/` | host harness: pack and render to a file, print the numbers |
| `src/device/` | the frame: settings in NVS, WiFi, the web UI and captive portal, the keys, the sleep loop |
| `tools/webflash.py` | Python: build the images and serve `webflash/` as an ESP Web Tools page, so the board flashes from Chrome/Edge |
| `tools/subset_font.py` | Python: the two faces cut down to what the frame draws |
| `tools/export_web_plates.py` | Python: the card packs split into one file a species, for the web |
| `tools/planecoder.py` | Python: the packs' context-model range coder, and `--train` to relearn the luma prior (`lib/plates/plane_prior.h`) |
| `packs/` | the baked plate packs, one per board and region; baked on the workstation that holds the artwork, which is not in the repository |

Two environments because the packer and the dither are the risky parts and
hardware is the slow way to find that out. `native` makes a layout or a colour
bug a one-second loop; `xiao` answers the questions only the real chip can.

## What the frame does

Wake, do one thing, sleep. On the timer it joins WiFi, sets the clock from NTP,
asks the source for recent sightings, keeps the ones it has a plate for, packs
them, dithers the page and pushes it to the glass - about 40 seconds awake, 30
of them the panel's own refresh - then deep-sleeps for the interval (default an
hour, as short as a minute, none during the quiet hours). The interval runs
wake to wake: the time spent awake comes off the sleep, so a wake that
outlasts it goes straight into the next. Every page is a new layout of the
birds - the layout number moves on each time - so the same birds come out in
new places. Everything it learned is in NVS before it
sleeps, because deep sleep is a reboot.

**The three keys** (GPIO 2, 3 and 5, active low, all RTC-capable so they wake it):

- **Key 1** turns the settings portal on. WiFi stays up and the web UI answers
  at `http://birdposter.local/` (or the address on the status page) until
  *Done* is clicked or half an hour passes untouched. Pressing it again while
  it is on only restarts the half hour.
- **Key 2** toggles the status page on the glass: address, source, endpoint,
  whether the last fetch answered and with what, the settings in force, the
  clock, free memory, firmware version. Press again for the birds.
- **Key 3** draws a new page now, advancing the layout.

**No network yet**, or the configured one fails three wakes running, and the
frame becomes its own access point (`birdposter-XXXX`, password `birdposter`)
and draws the setup page: a WiFi QR code a phone's camera turns into a join
prompt, the name and password in text beside it, and the address. The portal
is captive - a DNS catch-all plus the Android/Apple/Windows connectivity
probes redirected - so joining opens the settings page by itself. With no
network configured at all it stays up indefinitely: there is nothing else to
do, and sleeping would only hide it. Once a network is saved and joined but
the source has nothing to ask yet - iNaturalist without a location, BirdNET-Go
without an address - the frame shows its status page on the glass with the
settings address and again stays reachable with no timeout; the moment the
settings are saved it carries straight on to a page. A portal opened with key
1 on a set-up frame keeps the half-hour idle timeout.

**The web UI** is one page: status and a preview of what is on the glass (the
last frame, served as a 4-bit BMP), then WiFi, source (iNaturalist, eBird or
the Atlas of Living Australia by lat/lng/radius/days, a BirdNET-Go address, or
the URL of a JSON list - only the chosen source's fields show), page (count, hang, names and their size), schedule (interval, quiet
hours, timezone), and actions - new page, status page, test pattern, setup
page, sleep, reboot, forget WiFi. **Test source** under the source fields
makes the request the next refresh would, with the values as typed rather
than as saved, and reports the URL, the HTTP status, how many species came
back and how many of those have a plate - so a wrong address or an empty
radius shows up in a browser tab, not as a blank refresh an hour later.

While a page is being made the card shows what the frame is doing - asking
the source, reading the reply, choosing, packing, drawing, dithering, writing
names, sending to the glass, refreshing - one line at a time. Everything in
that list blocks, so `App::progress()` runs the web server's poll between
stages and the panel's BUSY wait calls it too; saves and actions are refused
with a 503 until the page is done. The banner is driven from `/api/status`,
which carries the current `phase` and a `stamp` that changes when the frame
finishes.

The page works with scripts off; with them on it also looks a place up by name
(iNaturalist's `places/autocomplete`, called from the browser - the API
allows cross-origin reads) to fill the coordinates, offers a timezone picker
that writes the POSIX string, and after *Fetch and draw* polls `/api/status`
and reloads itself when the frame has finished. Passwords are never echoed
into the page: a blank field keeps what is saved. A save with a blank or
out-of-range coordinate, or `0, 0`, is refused rather than quietly pointed at
the Gulf of Guinea, and *Rarest* is coerced to *Most seen* for BirdNET-Go,
which has no global count to rank on. The status card says when the portal
will close and when the next page is due; the glass's status page carries the
same line.

**Cycle birds** (`cycleHours`, 0 off) stops the page being the same top few
every time: `choose()` still ranks everything drawable, then `cycle()` in
`lib/source` draws a random page from the birds not shown within the window
and tops up with the longest-unseen when that runs short. When each species
was last drawn is kept in `/shown.txt` on the plates filesystem - a few
hundred lines is too much for the 20 KB NVS, and losing it to a plates
re-flash costs nothing.

**The web flasher** (`tools/webflash.py`) is also published as the
repository's GitHub Pages site, <https://c4kew4lk.github.io/bird_poster/>, by
`.github/workflows/flasher.yml` on every push to `main` - the same script
run on a runner over the committed plate packs (`firmware/packs/`, rebaked
and committed with the artwork when it changes), so the latest build flashes
from a browser with nothing installed. A tag push also attaches every image
to the GitHub release.
The page has a picker for the board - the XIAO in the EE02, or the reTerminal
E1004 (`pio run -e e1004`: the same sources with its pin map, `BOARD_E1004`,
and `partitions-plates-32mb.csv`) - and, for the XIAO, for the artwork:
Australian, European or North American plates, one image each, a 16 MB
frame holding one. The E1004's image carries all three as
`/plates-<region>.bin` and the settings page chooses among them, or one
region alone with the whole partition, about one and a half times the sprite
size, chosen on the page instead; the E1004
also lists the same three packs at full size (1200 px, `firmware/packs/card/`)
as downloads to copy to its microSD card, which the frame then reads in
preference to the flash copy. It offers three installs: everything,
for a first flash with an erase; app only, for a firmware update that leaves
the plates and every setting alone; and plates only, for a new pack. Every
manifest sets `new_install_prompt_erase`: ESP Web Tools erases the whole chip
*by default* on a device without Improv Serial - `false` there means "erase
without asking", not "do not erase" - and that is what was wiping the
settings and the plates on every app update. With the prompt the checkbox
starts unticked. The app also goes as three images at their own offsets
rather than one merged image, since a merged image pads the gaps with 0xFF
and the NVS partition sits in one. The plates
partition is labelled `plates` in the table and mounted by that label -
`LittleFS.begin()`'s default looks for one called `spiffs`, and a frame that
reports *no filesystem* with the image plainly written is that.

## How a page is drawn

The pack (`tools/bake_plates.py`, format in its docstring) carries each
species as a 1-bit silhouette for the packer and a posterised sprite for the
panel, plus mass, label box and flip. Sprites are *not*
pre-dithered: a dither is only right at the size it is drawn at, and the
packer's scale search decides that at render time. A sprite is two planes,
luminance and chrominance posterised apart - JPEG's observation applied to a
palette: 15 luma levels a pixel, 16 (Cb, Cr) pairs one per 2x2 block, both
k-means with chroma weighted as the dither weights it. The chroma is fitted,
by least squares, to the blend the frame draws it with, not averaged per
block, and the 16 pairs go to the colours that differ rather than to the
largest fields. That is 240 colours a
sprite for about what 15 used to cost, and the difference is a cockatoo's
yellow crest or a kookaburra's rust tail surviving the bake. The sixteenth
luma code is "outside": that one plane is the sprite's shape for the packer
and its pixels for the renderer, and the feathered edge is a ramp toward
paper in the luma - transparency the dither never has to know about. A
sixteenth level was tried and could not be told from fifteen on the page; a
separate silhouette stream cost the same bytes as the runs of the sentinel
do. The paper halo the cut leaves outside the silhouette measured 0.0% of a
sprite's ink, so it is not stored. Both planes are range-coded
(`lib/plates/planecoder.cpp`, `tools/planecoder.py`), each code in the context
of its decoded neighbours - about two-thirds of zlib's bytes, which the pack
spends on the 2x2 chroma and a larger sprite. The reader expands the planes to a luma
code a pixel and a chroma pair a block, and a pixel's colour is made as it is
drawn, the chroma interpolated between block centres. A plate drawn larger
than it was baked is filled in by Catmull-Rom (bilinear below 1.5x, a box
filter when it shrinks by 2x or more), its outline still the packer's mask.
The page is composed on an RGB565 canvas (3.8 MB of the PSRAM) and dithered
to the six inks in a single serpentine Floyd-Steinberg pass, the colour boost
being a 64K-entry table over RGB565. The pass starts on a row run 64 times
over so the top of the page begins with the error it would have carried, and
each pixel's ink is chosen with a small repeatable nudge (`jitter`) that
breaks up the lattices error diffusion draws in flat tone; the error passed
on is the true one, so the average colour holds. Before that pass the canvas palette is
pushed - saturation about each colour's own luma, contrast about a pivot
near the paper - by the *Colour* setting (`vivid`, 0 least to 4 most): a
faithful reduction of a soft plate reads as washed out on six dull inks, and
stretching the source first spends more coloured ink per area. The preview
palette (`kInkRgb`) has been checked against the glass and is left alone;
the boost is on the input side only. `--vivid N` in the harness.

*Edges* (`edges`, 0 off to 4, default 2) is the other half of the same
problem: an unsharp mask only amplifies contrast an edge already has, so a
white bird's outline against the paper stays faint. A Sobel gradient over the
same rolling rows finds the boundary whatever its contrast, and ink is laid
along it on a square-root curve that lifts the faint ones, weighted by the
pixel's own lightness so it goes to the white bird and not into dark plumage
- the drawn line a lithograph had before the scan softened it. `--edges N`
in the harness. None of Colour, Detail or Edges is baked: all three run on
the frame in the dither pass, so a change in the web UI is the next page.

The dither measures distance against `kInkTarget`, not `kInkRgb`: the inks
scaled so the panel's white lands on the paper. The panel's white is a grey
(208) and the paper is not (242), and measured raw everything in a plate
lighter than 208 - a white cockatoo's shading, its grey outline - was
"brighter than white", clamped to pure white, and the bird vanished into the
page. The eye takes the panel's white for white, so the paper is the white
point. The boost keeps the paper fixed by the same reasoning: it normalises
the paper to neutral, stretches, and scales back.

Error diffusion is also a low-pass filter - a one-pixel line against paper
comes out as scattered dots - so the *Detail* setting (`sharpen`, 0 off to
4) runs an unsharp mask on the way into the same pass: a 3x3 mean from a
rolling three-row RGB buffer, no second canvas. It roughly doubles the
dither's time, which is a second or so on the board. `--sharpen N` in the
harness. Labels go on after the dither as solid black, from a
TrueType face (stb_truetype) at the size the packer reserved. A label is, by
default, the common name with the scientific name under it at 70% (*Names*
also offers just one or the other, or none) - the pack knows only
the scientific name, so the common one comes from the source with each fetch
(`preferred_common_name` from iNaturalist, `commonName` from BirdNET-Go) and
its two-line box is measured at render time in the device's own fonts; the
baked one-line box is the fallback when there is none. The common name is set
in `name.ttf` (Gould Condensed, an engraved inline face at 80% of Gould's width) and the scientific name, like
every other word on the glass, in `label.ttf` (Gentium Book Plus Italic);
without a `name.ttf` both lines use the label font. The host harness
fills them from a BirdNET label file when it has one (`--labels FILE`,
`--no-common`); without it the harness shows scientific names alone.

The pack has to fit the 13.94 MiB `plates` partition beside the fonts, which
go in subset (`tools/subset_font.py`: Gentium to Latin, 883 KB to 63; Gould
Condensed to capitals and a name's punctuation, `--caps`, 33 KB - a name it
cannot set falls back to the label face). The EE02 packs are baked to fill
what is left, 452-596 px on the long side by region; on a 10-bird page most
birds are drawn near that size, and a page of one or two birds fetches the
full-size plate from the web when it can (see *Full-size plates from the
web* above).

### The page

Beyond the birds and their names, all set from the web UI:

- **Date** - today's date along the top or bottom edge, left, centre or right,
  numeric (day or month first) or in words; the birds are packed around it.
- **Paper** - white, or four strengths of cream, multiplied over the page
  before the dither so the plates' own paper takes the same tone.
- **Margin** - a border the page draws nothing in, so a mount or a bezel over
  the glass does not cut a name off the edge. One figure for the page, or one
  a side for a rebate that is not even; in page pixels (1600 x 1200 whichever
  way the frame hangs), at most a quarter of the page a side. The birds are
  packed into what is left, so a margin draws them smaller rather than leaving
  a gap. 0, the default, is the glass itself: the birds bleed off it.
- **Scientific name size** - 100% down to 50% of the common name.
- **Shuffle the birds' order** - the chosen birds handed to the layout in a
  new random order every page, rather than the most seen (or rarest) first.
  The first bird takes the middle of the page, so this moves which bird gets
  it - with Hero, which bird is the hero.
- **Count refreshes** - a battery test: every refresh of the glass is counted,
  the running total is drawn small in the date's strip and on the status
  page, and it survives the battery going flat; reset from the status box.
- **Full-size plates from the web** - above.

## Where this is up to

Steps 1-4 of the build order in `IMPLEMENTATION.md` §8 are written and the
host half of them is tested. **None of it has run on the EE02 yet.** The parts
that only hardware can prove, in the order they will bite:

- the panel init sequence and framing are Seeed_GFX2's, and the pin map is
  the ESPHome port's - both hardware-verified upstream, neither here.
  `Actions → Test pattern` is the first thing to try: six bars, black at the
  top left, a strip of dithered paper, and a line of text.
- the SPI clock is 10 MHz, which is what Seeed ships; the ESPHome port found
  2 MHz its ceiling on its wiring. `kSpiHz` in `lib/panel/panel.cpp`.
- the keys are on GPIO 2/3/5 per the ESPHome port's reading of the schematic,
  pulled up internally in the RTC domain for wake. If a press does nothing,
  that is the first thing to meter.
- no battery measurement: the notes' §5 indicator needs an ADC pin the EE02
  has not been shown to spare. The status page says "not measured".

## The packer against the Python

The port is checked against the Python renderer rather than trusted. On the same
ten species at 1600×1200:

| | names off | names on |
| --- | --- | --- |
| bird median, Python / C++ | 375 / 375 px | 316 / 325 px |
| label used, Python / C++ | – | 26 / 27 px |
| ink covered | 39% | 28% / 30% |
| Python | 1139 ms | 2513 ms |
| C++ | 26 ms | 34 ms |

The C++ row was re-measured after the bitset rewrite in *What the pack costs*
below; the same ten species on the same machine took 114 ms and 144 ms before
it. Only that row moved. Everything above it describes where the birds land,
which the rewrite leaves alone - the harness writes a byte-identical PGM either
way, and `pio test -e native` pins that.

**The two no longer agree exactly, and that is a deliberate trade.** They did
while the scale search walked a fixed 0.9 ladder - both landed on the same rung.
Bisecting lands on arbitrary scales instead, where two approximations in this
port start to show: masks rescale nearest-neighbour where the Python uses
LANCZOS, and label boxes scale linearly from one measurement where the Python
re-rasterises the font. The result is about 3% on bird size. Worth knowing before
reading a difference as a bug.

Both bugs found by this comparison were the same mistake - sizing off the packing
box rather than the page, once for the birds and once for the labels. The box is
8% smaller on the short side, which is enough to converge the whole layout
somewhere else.

**Birds grow into the gaps after the pack.** The scale search grows the
whole set together and stops at the size the tightest bird runs out of room,
which leaves the others smaller than the space beside them allows. `grow()`
(`lib/packer`) then takes each placed bird, smallest first, and bisects it
up toward 1.5x where it stands - centre held, then each edge held in turn -
keeping the largest that fits the box and touches nothing. Names stay at the
packed size. On the Canberra portrait page this took the ink from 17.4% of
the paper to 20.2% with nothing moved. It is a separate pass so `layout()`
stays pinned against its reference; `--no-grow` in the harness shows the
packer alone.

**Names are packed with their birds, not placed afterwards.** `withLabel` joins
the reserved box into the bird's own collision mask, which is what guarantees a
name a place at all - the packer leaves no free paper between birds, so a name
added later could only sit outside the cluster and every interior bird would get
none. The harness draws the reserved boxes so the reservation can be seen.

The bitset is where the speed lives. A 1200×1200 occupancy grid is 176 KB rather
than the 1.4 MB a byte-per-pixel array would need, and a collision test is a
word-wise AND with early exit: 26 ms on the host against the Python's 1139 ms
for the same ten birds.

### What the pack costs

Two thirds of it used to be mask preparation rather than packing, which was not
where anyone would look. Measured on the ten-bird page, per layout: the erosion
alone ran 103 million per-pixel operations for 44% of the time, the rescale and
the label's centroid scan another 31 million between them. All three are now
word-wise, and the erosion in particular is a shift-and-AND - 64 columns to the
instruction. It went from 44% of the layout to about 1%.

That leaves the search, and two things cut it:

- **The sweep stops where the page does.** The spiral ran out to the canvas
  diagonal, but a candidate is only on the page while the sweep's ellipse
  clears the sprite's own margin, and past that radius no angle is left that
  fits. The scale search deliberately opens with a set too big for the page, so
  those wasted laps were being walked several times per layout. 2.4M candidate
  positions considered became 830k.
- **The full footprint check reads from the edges inward.** By the time a
  candidate reaches it, it has passed three probe rows and is still colliding
  998 times in 1000, so all that matters is reaching the pixel that hits - and
  that pixel is at an edge, because the spiral walks a bird outward until it
  stops overlapping its neighbours. Densest-first, the obvious guess, measured
  14× worse: a mid-body row is the part still in clear paper.

Everything here is an optimisation and nothing else. The tests hold each
rewritten routine against the per-pixel version it replaced, bit for bit, and
pin a whole layout to literal coordinates, because a page that moved would
invalidate the fidelity table above rather than improve on it.

None of it changes the memory: the extra tables are a 512-byte constant in flash
and one row of column indices.

### Sizing by ink, not by box

`inkWeights` multiplies into `sizeWeights`. A weight alone sizes a bird by its
bounding box, and a plate that draws its bird among grass stems fills a fraction
of that box - measured on the Australian plates, a fairywren fills 39% where a
cockatoo fills 73%, so the packer hands them the same square and the wren reads
half the size. Correcting to equal *drawn* area is what makes the mass weighting
mean what it says. `kInkExponent = 1.0`; coverage floored at 2% so a nearly empty
mask cannot ask for an unbounded scale-up. Free on device: the masks are already
bitsets, so this is a sum of `rowPopcount`.

### Layout variants

`--layout N` (native) and the `variant` argument to `layout()` draw a different
arrangement of the same birds. Two levers, neither costing packing quality:

- **`turnFor`** offsets where the elliptical sweep starts. The spiral tries
  candidates in angle order, so the first bird placed at each radius lands where
  the sweep begins; moving that start rearranges the cluster while every
  candidate is still tried. Stepped by the golden angle, so successive layouts
  land far apart rather than cycling through a few arrangements.
- **`flipFor`** decides mirroring from the bird's own name *and* the layout, so
  an arriving bird still cannot re-roll its neighbours.
- **The packers' seed.** Grid, Scattered and Hero break ties from a seed; the
  layout picks which run of seeds (layout 0 is the seeds it always used), so
  those styles rearrange too rather than only flipping birds.

Ten Australian plates at 1600×1200, same set:

| layout | ink | cluster span | pack |
| --- | --- | --- | --- |
| 0 | 21.6% | 71.2% | 25 ms |
| 1 | 20.5% | 79.1% | 30 ms |
| 2 | 20.5% | 74.5% | 29 ms |

Re-measured on the current plates, which have themselves changed since this
table was first written - the ink and span columns move with the artwork, not
with the packer. The pack column was 102, 111 and 112 ms before the rewrite.

**One deliberate divergence from the Python.** Layout 0 uses the flip baked into
the mask file, which is the Python's blake2b answer, so the reference page stays
byte-identical and the fidelity table above still means something. Later layouts
use a cheap FNV-1a over the same name, because blake2b on device would cost more
than the variety is worth. The *rule* is the port; the digest is not.

The layout number must never be what triggers a render - on the Pi the trigger
key is computed with it pinned, and it advances only once a render is already
due. An e-ink refresh is the expensive thing, not the pack. The frame advances
it at the start of every page it draws, so each page is a new arrangement; a
consequence is that the skip of a page identical to the one on the glass no
longer fires, since the layout is part of what makes it identical.

## Known-rough

- Masks rescale with nearest-neighbour where the Python uses LANCZOS and
  re-thresholds. On a binary silhouette the two differ only along the outline
  and the erosion eats the difference, but it is an approximation.
- Label boxes are measured once at 100 px when baked and scaled linearly, where
  the Python re-rasterises the font at each size. It agrees on this ten-bird
  page; a face with heavier hinting could drift.
- The spiral search still probes blind within the radius it now stops at, so a
  full page costs far more than a sparse one. A skyline or bottom-left fill
  would cut the candidate set by an order of magnitude and, if anything, nestle
  tighter. This is the biggest remaining win in the packer.
- The rescale is the one hot loop that did not go word-wise: nearest-neighbour
  is a gather with a non-uniform stride, so it dropped only to a table lookup
  per pixel. About 8% of a layout.
- `kAttempts` discards what each pass learns. A binary search on the scale would
  converge in about five passes rather than up to twenty.
