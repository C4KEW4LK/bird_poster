"""Subset a label font to what the frame can draw, for the plates partition.

    .venv-rembg/bin/python firmware/tools/subset_font.py in.ttf out.ttf
    .venv-rembg/bin/python firmware/tools/subset_font.py --caps in.ttf out.ttf

Gentium Book Plus Italic is 883 KB because it covers most of Unicode; the
frame sets Latin - names, the status page, the setup page - and nothing else.
Basic Latin, Latin-1, Latin Extended-A and the general punctuation block
(curly quotes, dashes, the ellipsis) cover every string the firmware can
produce, and cut the file to about a tenth. The room goes to the plates.

`--caps` is for the common-name face, which the frame only ever sets in
capitals: the capital letters of the same blocks - Rüppell's, Marañon, the
macrons of the New Zealand names - the punctuation a bird's name carries, and
what the page's own lines put round a time, a battery level or a temperature
(: % & ! ? + # @ ; and the degree sign), nothing lower-case. A name the subset cannot set falls back to the label face
on the frame, so a stray character costs a change of face, not a box.

Either way only the kerning survives of the OpenType features: the frame's
rasteriser applies nothing else, and keeping the rest keeps every small cap,
swash and ligature the features can reach - most of the file.
"""

from __future__ import annotations

import sys
from pathlib import Path

from fontTools import subset

RANGES = "U+0020-007E,U+00A0-00FF,U+0100-017F,U+2010-2027,U+2030-203A,U+2122,U+00B0"


def caps_unicodes() -> list[int]:
    """Capitals, digits, a name's punctuation - space ' ( ) , - . / and the
    curly apostrophe and dashes a web source may send - and the page text's."""
    keep = [ord(c) for c in " '(),-./0123456789:%&!?+#@;\u00b0"]
    keep += list(range(ord("A"), ord("Z") + 1))
    keep += [c for c in range(0xC0, 0x180) if chr(c).isupper()]
    keep += [0x178, 0x2010, 0x2011, 0x2013, 0x2014, 0x2018, 0x2019]
    return sorted(set(keep))


def subset_font(src: Path, dst: Path, caps: bool = False) -> None:
    opts = subset.Options()
    opts.layout_features = ["kern"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    font = subset.load_font(str(src), opts)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=caps_unicodes() if caps else subset.parse_unicodes(RANGES))
    sub.subset(font)
    dst.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(dst), opts)


if __name__ == "__main__":
    args = sys.argv[1:]
    caps = "--caps" in args
    args = [a for a in args if a != "--caps"]
    if len(args) != 2:
        raise SystemExit(__doc__)
    subset_font(Path(args[0]), Path(args[1]), caps)
    print(f"{args[1]}: {Path(args[1]).stat().st_size / 1024:.0f} KB")
