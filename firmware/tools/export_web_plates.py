"""Split the full-size plate packs into one file a species, for hosting.

    python3 firmware/tools/export_web_plates.py                       # every region, under the key
    python3 firmware/tools/export_web_plates.py --deploy fragrant-heart-ad53   # ... and upload with wrangler
    python3 firmware/tools/export_web_plates.py --new-key --deploy fragrant-heart-ad53   # a fresh key

The site is built in firmware/webplates/site/<key>/<region>/: the plates sit in a
folder named by a random key, so only a frame told the address can find them.
The key is made on the first run and kept in firmware/webplates.key, which git
ignores - it is never in the code, the repository or the web flasher. The
frame's URL is then https://<host>/<key>/{region}, entered in its web UI.
`--deploy WORKER` uploads the site as that Cloudflare Worker's static assets
with `npx wrangler deploy` and keeps --host attached to it as its custom domain
(log in once with `npx wrangler login`). birdpngs.c4k3.xyz is the assets-only
Worker fragrant-heart-ad53 - a Worker rather than Pages, which is what the
dashboard's drag-and-drop made, and why that upload stopped at 1,000 files;
Wrangler takes up to 20,000. A deployment replaces every asset, so nothing
outside the keyed folder stays.

The frame's flash holds each region at the size its partition allows (about
400 px on an EE02). When a bird is drawn much larger than that - a page of one
or two birds - the frame asks the web for the same plate at full size and
falls back to the one in flash if the site does not answer. This writes what
it asks for:

    <out>/<region>/<Scientific_name>.bin    one sprite ('FGPS' v1)
    <out>/<region>/index.json               the names and the size baked at

from `firmware/packs/card/<region>.bin`, the E1004's SD-card packs, which are
the same bake at 1200 px. Upload to the site the frame's "Full-size plates
from the web" setting points at; any static host serving the files as they are
will do. The frame asks for <url>/<Scientific_name>.bin, with a "{region}" in
the URL replaced by the plates' region: upload one region's folder to a site's
root, or <out> whole and put /{region} in the URL. The name in the file name is
the scientific name with spaces as underscores; the frame percent-encodes the
rest.

FGPS v1, little-endian: magic u32 'FGPS', version u32 = 1, source u16, w u16,
h u16, luma u8[16], cb i8[16], cr i8[16], luma_len u32, chroma_len u32, then
the zlib luma plane and the zlib chroma plane exactly as a pack record holds
them (see bake_plates.py).
"""

from __future__ import annotations

import argparse
import json
import secrets
import shutil
import struct
import subprocess
from datetime import date
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1]
PACK_MAGIC, PACK_VERSION = 0x4C504746, 6
SPRITE_MAGIC, SPRITE_VERSION = 0x53504746, 1


def read_pack(path: Path):
    """The header and index of an FGPL v6 pack, and where its payload starts."""
    data = path.read_bytes()
    magic, version, depth, source, count = struct.unpack_from("<IIBHI", data, 0)
    if magic != PACK_MAGIC or version != PACK_VERSION or depth != 4:
        raise SystemExit(f"{path}: not an FGPL v6 pack")
    at = 15 + 3  # header, then the paper tone
    records = []
    for _ in range(count):
        (name_len,) = struct.unpack_from("<H", data, at)
        name = data[at + 2 : at + 2 + name_len].decode()
        at += 2 + name_len
        w, h, _flip, alias, _lw, _lh = struct.unpack_from("<HHBBHH", data, at)
        at += 10
        tables = data[at : at + 48]
        at += 48
        offset, luma_len, chroma_len = struct.unpack_from("<III", data, at)
        at += 12
        records.append((name, w, h, alias, tables, offset, luma_len, chroma_len))
    return data, source, records, at


def export(pack: Path, out: Path) -> int:
    data, source, records, payload = read_pack(pack)
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for name, w, h, _alias, tables, offset, luma_len, chroma_len in records:
        streams = data[payload + offset : payload + offset + luma_len + chroma_len]
        head = struct.pack("<IIHHH", SPRITE_MAGIC, SPRITE_VERSION, source, w, h)
        head += tables + struct.pack("<II", luma_len, chroma_len)
        (out / (name.replace(" ", "_") + ".bin")).write_bytes(head + streams)
        names.append(name)
    (out / "index.json").write_text(
        json.dumps({"source": source, "count": len(names), "names": names}, indent=1) + "\n"
    )
    return len(names)


KEY_FILE = FIRMWARE / "webplates.key"


def site_key(new: bool) -> str:
    """The folder name the plates hide under: made once, kept out of git."""
    if new or not KEY_FILE.exists():
        KEY_FILE.write_text(secrets.token_urlsafe(18) + "\n")
        KEY_FILE.chmod(0o600)
    return KEY_FILE.read_text().strip()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--packs", type=Path, default=FIRMWARE / "packs" / "card")
    ap.add_argument("--out", type=Path, default=FIRMWARE / "webplates" / "site")
    ap.add_argument("--regions", default="", help="comma-separated; default every pack found")
    ap.add_argument(
        "--new-key", action="store_true", help="make a fresh key (the old address stops working)"
    )
    ap.add_argument(
        "--deploy", metavar="WORKER", help="upload the site as this Cloudflare Worker's assets"
    )
    ap.add_argument(
        "--host", default="birdpngs.c4k3.xyz", help="for the address printed at the end"
    )
    args = ap.parse_args()
    regions = [r for r in args.regions.split(",") if r] or sorted(
        p.stem for p in args.packs.glob("*.bin")
    )
    key = site_key(args.new_key)
    # The site is only ever this key's folder: anything else - an old key's
    # folder, files from before there was a key - goes, so a deploy removes it.
    shutil.rmtree(args.out, ignore_errors=True)
    for region in regions:
        pack = args.packs / f"{region}.bin"
        if not pack.exists():
            raise SystemExit(f"no {pack}")
        folder = args.out / key / region
        n = export(pack, folder)
        size = sum(f.stat().st_size for f in folder.glob("*.bin"))
        print(f"{region}: {n} sprites, {size / 1048576:.1f} MB")
    files = sum(1 for f in args.out.rglob("*") if f.is_file())
    print(f"site: {files} files in {args.out}")
    if args.deploy:
        subprocess.run(
            [
                "npx",
                "--yes",
                "wrangler@4",
                "deploy",
                "--name",
                args.deploy,
                "--assets",
                str(args.out),
                "--compatibility-date",
                date.today().isoformat(),
                "--domain",
                args.host,
            ],
            check=True,
            cwd=args.out.parent,  # no wrangler config there to be picked up by accident
        )
    print(
        f"frame URL (enter it in the frame's web UI; keep it private):\n"
        f"  https://{args.host}/{key}/{{region}}"
    )


if __name__ == "__main__":
    main()
