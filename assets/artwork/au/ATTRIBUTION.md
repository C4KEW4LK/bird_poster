# au - attribution and licensing

Australian bird plates, cut and edited for this project, so none is faithful to
its scan. The style is offered under **CC BY-SA 4.0**, the most restrictive of
the sources below:

> Digital restorations, edited for this project.
> [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

`manifest.json` links each file to the plate it came from, and records the
licence that plate carried on Commons.

## Sources

**Gould** - *The Birds of Australia* (1840-48) by **John Gould** (1804-1881),
lithographed by **Elizabeth Gould** and **H. C. Richter**, from the Commons
category
[The Birds of Australia (John Gould)](https://commons.wikimedia.org/wiki/Category:The_Birds_of_Australia_(John_Gould)).
Most files are rawpixel's digitally enhanced scans, which Commons carries as
CC BY-SA 4.0; the rest are tagged public domain. Per-file licences are in
`manifest.json`.

**The rest** - one plate apiece, for the 156 birds recorded in Australia that
Gould never drew: the introduced birds, the seabirds and waders of his blind
spots, and a few he painted so badly the plate was passed over. Taken from
wherever Wikimedia Commons had a coloured plate of the species - Keulemans in
Lilford's *Coloured Figures of the Birds of the British Islands* and in
Buller's *Birds of New Zealand*, Broinowski's *Birds of Australia* (1890),
Gould's own *Birds of Europe* and *Birds of Asia*, Audubon, Smit, Grønvold in
*The Emu*, Huet and Prêtre, and the Biodiversity Heritage Library's scans of
the rest. `manifest.json` records each one's `source` as `commons`, with the
Commons page, the artist credited there and the licence that page states:
public domain for most, CC BY 2.0, CC BY 4.0 and CC BY-SA 3.0/4.0 for a few.
The `ship` stage owns only Gould's plates and leaves these alone - except that
when Gould's plate of a species ships, the other plate of it goes.

## Scope

Every species Gould drew that could be resolved to a current name is kept, and
the supplementary plates fill in behind them, one per species. The
frame looks artwork up by the scientific name rather than by the detector, so a
plate BirdNET v2.4 has no label for is still a usable plate - another source can
reach it, and 335 of the 871 bird species recorded in Australia have no v2.4
label at all. Those names are listed in `species-extra.txt`, each vouched for by
iNaturalist or GBIF. Gould's 1840s binomials were resolved to modern names first;
`notes/australian-artwork/research/gould-aliases.tsv` records how, and cites a
source per row.
