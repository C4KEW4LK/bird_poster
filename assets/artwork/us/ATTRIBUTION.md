# us - attribution and licensing

North American bird plates, cut and edited for this project, so none is
faithful to its scan. The style is offered under **CC BY-SA 4.0**, the most
restrictive of the sources below:

> Digital restorations, edited for this project.
> [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

`manifest.json` links each file to the plate it came from, and records the
licence that plate carried on Commons.

## Sources

**Audubon** - *The Birds of America* (1827-38) by **John James Audubon**
(1785-1851), engraved and coloured by **Robert Havell Jr.**, from rawpixel's
digitally enhanced scans on Wikimedia Commons, which Commons carries as
CC BY-SA 4.0 (433 files titled "Illustration from Birds of America (1827) by
John James Audubon, digitally enhanced by rawpixel-com N"). Per-file licences
are in `manifest.json`.

**The rest** - one plate apiece, for the 155 birds recorded in the United
States that Audubon never drew: the western birds his travels missed, the
Hawaiian and introduced birds, and the species split off since. Taken from
wherever Wikimedia Commons had a coloured plate of the species - Keulemans in
Lilford, Buller and the *Ibis*, Gould's *Birds of Europe*, *Birds of Asia* and
the *Monograph of the Trochilidae*, Cassin's *Illustrations of the Birds of
California, Texas, Oregon...*, Baird's *Pacific Railroad Reports*, Fuertes,
Brooks in Dawson's *Birds of California*, Sharpe, Smit, Frohawk, and the
Biodiversity Heritage Library's scans of the rest. `manifest.json` records
each one's `source` as `commons`, with the Commons page, the artist credited
there and the licence that page states: public domain or "no restrictions"
for nearly all, CC0, CC BY 2.0 and CC BY-SA 3.0/4.0 for a handful. The `ship`
stage owns only Audubon's plates and leaves these alone - except that when
Audubon's plate of a species ships, the other plate of it goes.

## Scope

397 of the 433 plates, 367 species. Audubon's plates name their birds by his
own English and nothing else, so each was resolved to a modern species through
the National Audubon Society's plate pages, which link every plate to its
field-guide entry, with the extinct birds and Audubon's juvenile warblers
painted as new species resolved by hand; `scripts/names/audubon-aliases.tsv`
has every row with its source. Left out: the 24 plates with several species on
them, which nothing can separate, and a dozen birds that never existed or whose
identity is disputed - the Bird of Washington, the Carbonated Warbler and their
like - listed in the same file. The six extinct birds Audubon painted are kept,
vouched in `species-extra.txt`.

Audubon's compositions are dense - a bird among leaves, flowers or grass - and
the cut keeps that scene, as the project's rule has it. Where the matting
painted bare paper into the gaps between leaves it was carved back out by
colour (`cut --max-hole 0.005`), and those plates are listed in
`scripts/names/ship-accept.tsv` because the outline check cannot tell a leaf
edge from a shred.
