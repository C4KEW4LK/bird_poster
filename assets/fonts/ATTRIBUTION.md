# Attribution and licensing

Two typefaces go onto the page: the scientific name in **Gentium Book Plus
Italic** - a name is set in italic by convention, and this face keeps its
hairlines at label size on a six-colour panel - and the common name in
**Gould Condensed**, an upright display face made for this project after the
caption lettering on Gould's plates. Both are under the **SIL Open Font License 1.1**; each ships
with its own `OFL.txt` in its directory, which is the copyright notice the
licence requires be distributed with the font.

The OFL covers the font files only. It is not viral: it places no conditions on
the images the fonts are used to render, so the collage's licensing is
unaffected.

## Families

| Directory | Family | Upstream |
| --- | --- | --- |
| `gentiumbookplus/` | Gentium Book Plus | [SIL International](https://software.sil.org/gentium/) |
| `gould/` | Gould Condensed | derived from Playfair Display, see below |

Gentium is taken from the [Google Fonts](https://github.com/google/fonts)
distribution; only the italic is vendored. Both faces are subset before they
go on the frame (`firmware/tools/subset_font.py`): Gentium to Latin, Gould
Condensed to capitals and a name's punctuation.

## Gould Condensed

`gould/GouldCondensed-Regular.ttf` is an upright display face made for this
project, not vendored: Playfair Display at weight 700, stretched 1.04× wide and
hollowed along every thick stroke. The model is the caption lettering on
Gould's *Birds of Australia* plates - open-face capitals with a hairline on the
left of every stroke and a heavier wall on the right, the way the burin shaded.
Every stroke thick enough to hold its walls is cut through; hairlines,
crossbars and serifs are left solid, and a stem's cut runs square into its
serifs. (A wider cut, Gould, stretched 1.3×, came first; the condensed one is
what the frame uses.)

It is a Modified Version under the OFL and ships under the same licence, with
Playfair's notice in `gould/OFL.txt`. "Playfair Display" is a Reserved Font
Name, which is why the derivative carries a name of its own.
