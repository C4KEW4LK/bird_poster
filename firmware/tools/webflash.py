"""Serve the frame's firmware as an ESP Web Tools page, for flashing from a browser.

    python3 firmware/tools/webflash.py --bake       # build every board, bake what is missing, serve on :8000
    python3 firmware/tools/webflash.py --no-serve   # just assemble webflash/dist/
    python3 firmware/tools/webflash.py --boards ee02 --regions au   # one board, one plate image

The same page is published at https://c4kew4lk.github.io/bird_poster/ by
.github/workflows/flasher.yml on every push to main, built the same way from
the same script, so the latest build can be flashed from a browser without
cloning anything. Pages is HTTPS, which is what Web Serial needs.

Two boards carry the same panel:

  ee02   a XIAO ESP32-S3 Plus in Seeed's EE02 driver board, 16 MB of flash.
         Its plates partition holds one regional pack, so there is one plate
         image per region and the page has a toggle for which to flash.
  e1004  Seeed's reTerminal E1004, 32 MB of flash. One plate image carries
         every region, and the region is a setting on the device.

Four images go to a board, each at its own offset: bootloader, partition table
and app as `pio run -e <board>` leaves them, and the plates - the LittleFS image
`pio run -t buildfs` makes from firmware/data/, the baked packs and the fonts -
at the `plates` partition's offset, read out of that board's partition table so
the two cannot drift apart. They are deliberately not merged into one image:
the NVS partition with every setting lives in the gap between the partition
table and the app, and a merged image writes 0xFF over the gaps.

Each board gets three manifests - everything (a first install, with an erase),
the app alone (an update that leaves the plates and settings in place) and the
plates alone - and the page reads `builds.json`, written here, to know what to
offer. Packs are baked per board because the budgets differ: the XIAO's one
pack may have about 13.6 MiB, the E1004's three share 31 MB, and the E1004's SD
card takes each region at full size (`firmware/packs/card/`). `--bake` bakes any
pack that is missing (minutes each).

Each plate image is staged in its own directory under .pio/webflash-data/ -
the fonts subset afresh from assets/, and exactly that image's packs - and built
from there, so what goes on a board is decided here and nowhere else:
firmware/data/, which `pio run -t uploadfs` flashes by hand, is left alone.

The site also carries the full-size plates the frame fetches for a bird drawn
much larger than its plate in flash: plates/<region>/<Scientific_name>.bin,
split from the card packs by export_web_plates.py. Published on GitHub Pages,
that is the frame's default web-plates address,
https://c4kew4lk.github.io/bird_poster/plates/{region}.

Web Serial only works over HTTPS or on localhost, so the page is served from
here rather than published anywhere else: open it in Chrome or Edge on the
machine the board is plugged into, or forward the port there.
"""

from __future__ import annotations

import argparse
import csv
import http.server
import json
import os
import shutil
import struct
import subprocess
import sys
from datetime import date
from pathlib import Path
from typing import Any

FIRMWARE = Path(__file__).resolve().parents[1]
ROOT = FIRMWARE.parent


def tool(name: str, venv: str) -> Path:
    """A tool from its gitignored venv on the workstation, or from the PATH in
    CI, where the workflow installs it into the environment it runs in."""
    local = ROOT / venv / "bin" / name
    if local.exists():
        return local
    found = shutil.which(name)
    if found:
        return Path(found)
    raise SystemExit(f"no {name}: not in {venv}/ and not on the PATH")


PIO = tool("pio", ".venv-pio")
FONT = ROOT / "assets" / "fonts" / "gentiumbookplus" / "GentiumBookPlus-Italic.ttf"
NAME_FONT = ROOT / "assets" / "fonts" / "gould" / "GouldCondensed-Regular.ttf"  # the common name
PACKS = FIRMWARE / "packs"  # baked packs, per board; outside data/, which is the image

# region key -> (label, artwork style). `us` joins when Audubon's plates ship.
REGIONS = {
    "au": ("Australia", "au"),
    "eu": ("Europe", "eu"),
    "us": ("North America", "us"),
}

# board key -> how it is built and what its plates partition holds. `budget`
# is the MB a single pack may take: the whole partition less the fonts on the
# XIAO, a third of it on the E1004.
BOARDS: dict[str, dict[str, Any]] = {
    "ee02": {
        "label": "XIAO ESP32-S3 Plus in the EE02",
        "env": "xiao",
        "partitions": "partitions-plates-16mb.csv",
        "packs_per_image": 1,
        # The partition is 13.94 MiB; mklittlefs fits a 13.80 MiB pack beside
        # the subset fonts (about 96 KB), and ~150 KB is left for the frame to
        # rewrite /shown.txt. The baker aims at 98% of this: ~13.6 MiB.
        "budget": 13.9,
    },
    "e1004": {
        "label": "reTerminal E1004",
        "env": "e1004",
        "partitions": "partitions-plates-32mb.csv",
        "packs_per_image": 3,
        "budget": 10.2,
        # And a second kind of image: one region with the whole partition to
        # itself, which buys about 1.5x the sprite size (600-760 px against
        # 400) at the cost of choosing the region at flash time. Baked as
        # its own board key, since the budget is what a pack is baked to.
        "alone": {"key": "e1004-one", "budget": 30.0},
        # The E1004 also reads a pack from its SD card, where the plates keep
        # the full size they shipped at rather than the 400 px that fits a
        # third of the flash. Those packs are baked separately and offered as
        # files to copy to the card, not flashed.
        "card": True,
    },
}

# The SD card's packs: one per region, at the size the artwork was shipped at
# (1200 px on the longest side). No budget - a card is measured in gigabytes.
CARD_SOURCE = 1200


def pio(env: str, *args: str, data_dir: Path | None = None) -> None:
    run_env = dict(os.environ)
    if data_dir is not None:
        run_env["PLATFORMIO_DATA_DIR"] = str(data_dir)
    subprocess.run([str(PIO), "run", "-e", env, *args], cwd=FIRMWARE, check=True, env=run_env)


def plates_offset(board: str) -> int:
    """Where the `plates` partition starts, from the table the board flashes."""
    table = FIRMWARE / BOARDS[board]["partitions"]
    with table.open() as fh:
        for row in csv.reader(line for line in fh if not line.startswith("#")):
            if row and row[0].strip() == "plates":
                return int(row[3].strip(), 16)
    raise SystemExit(f"no `plates` partition in {table.name}")


def app_parts(build_dir: Path, dist: Path) -> list[dict]:
    """The app as three images at their own offsets, as `pio run -t upload`
    writes them. Not merged into one: esptool's merge_bin fills the gaps with
    0xFF, and the NVS partition that holds every setting - the WiFi, the
    location, all of it - sits in the gap between the partition table at
    0x8000 and the app at 0x10000. A merged image blanked it on every app
    flash. boot_app0 is left out too: it is OTA bookkeeping, this table has no
    OTA slots, and the offset it is written to is the RF calibration data."""
    parts = []
    for name, offset in (
        ("bootloader.bin", 0x0),
        ("partitions.bin", 0x8000),
        ("firmware.bin", 0x10000),
    ):
        shutil.copy(build_dir / name, dist / name)
        parts.append({"path": f"{dist.name}/{name}", "offset": offset})
    return parts


def tool_python() -> Path:
    """The interpreter with PIL, numpy and fonttools: the rembg venv on the
    workstation, the one running this in CI."""
    local = ROOT / ".venv-rembg" / "bin" / "python"
    return local if local.exists() else Path(sys.executable)


def pack_source(path: Path) -> int:
    """The sprite size a pack was baked at, from its header: what the page
    tells people an image buys them. (FGPL v6: magic, version, depth u8,
    source u16.)"""
    with path.open("rb") as fh:
        head = fh.read(11)
    return struct.unpack("<H", head[9:11])[0] if len(head) == 11 else 0


def pack_for(board: str, style: str, bake: bool) -> Path:
    """The baked pack for a style at this board's budget - or, for `board`
    "card", at full size - baking it when asked and it is missing."""
    path = PACKS / board / f"{style}.bin"
    if path.exists():
        return path
    if board == "card":
        args = ["--source", str(CARD_SOURCE)]
    else:
        budgets = {k: v["budget"] for k, v in BOARDS.items()}
        budgets.update(
            {v["alone"]["key"]: v["alone"]["budget"] for v in BOARDS.values() if "alone" in v}
        )
        args = ["--fit", "--budget", str(budgets[board]), "--source", "1200"]
    if not bake:
        raise SystemExit(
            f"no {path.relative_to(ROOT)} - bake it first (minutes), or pass --bake:\n"
            f"  {tool_python()} firmware/tools/bake_plates.py {path.relative_to(ROOT)}"
            f" --style {style} --count 0 {' '.join(args)}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(tool_python()),
            str(FIRMWARE / "tools" / "bake_plates.py"),
            str(path),
            "--style",
            style,
            "--count",
            "0",
            *args,
        ],
        check=True,
    )
    return path


def fonts_into(data: Path) -> None:
    """The two label faces, subset to Latin: the full Gentium is 883 KB and the
    frame draws none of the rest of Unicode. Always from the sources in assets/,
    so an image never carries a stale subset."""
    data.mkdir(parents=True, exist_ok=True)
    # The common name is only ever set in capitals, so its face keeps only those.
    for src, name, flags in ((FONT, "label.ttf", []), (NAME_FONT, "name.ttf", ["--caps"])):
        subprocess.run(
            [
                str(tool_python()),
                str(FIRMWARE / "tools" / "subset_font.py"),
                *flags,
                str(src),
                str(data / name),
            ],
            check=True,
        )


def filesystem_image(board: str, packs: dict[str, Path], out: Path, fonts: Path) -> None:
    """A LittleFS image of the fonts and the given packs, built with the board's
    partition table so it is exactly the partition's size. Packs go in as
    /plates.bin when there is one - the name a 16 MB board mounts - and as
    /plates-<region>.bin each when there are several. Staged in a directory of
    its own, so nothing else can end up in the image."""
    stage = FIRMWARE / ".pio" / "webflash-data" / f"{board}-{out.stem}"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    for font in fonts.glob("*.ttf"):
        shutil.copy(font, stage / font.name)
    if len(packs) == 1:
        region, pack = next(iter(packs.items()))
        shutil.copy(pack, stage / "plates.bin")
        # A lone /plates.bin cannot say which region it is; the frame reads this
        # to know where to ask the web for a plate's full-size version.
        (stage / "region.txt").write_text(region + "\n")
    else:
        for region, pack in packs.items():
            shutil.copy(pack, stage / f"plates-{region}.bin")
    pio(BOARDS[board]["env"], "-t", "buildfs", data_dir=stage)
    shutil.copy(FIRMWARE / ".pio" / "build" / BOARDS[board]["env"] / "littlefs.bin", out)
    shutil.rmtree(stage, ignore_errors=True)


def assemble(dist: Path, boards: list[str], regions: list[str], bake: bool) -> None:
    # dist is entirely this function's output; nothing else may live in it,
    # or it gets served with the page.
    shutil.rmtree(dist, ignore_errors=True)
    dist.mkdir(parents=True, exist_ok=True)

    sha = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    # A tag when the build is one, else the date and the commit: what the page
    # shows above the buttons, and what the frame's status page repeats.
    version = os.environ.get("BUILD_VERSION") or f"{date.today().isoformat()} {sha}"

    def manifest(name: str, chosen: list[dict]) -> dict:
        return {
            "name": f"Bird poster - {name}",
            "version": version,
            # Always prompt. ESP Web Tools erases the whole chip by default on
            # a device without Improv Serial: `false` here does not mean "do
            # not erase", it means "erase without asking" - which is what was
            # wiping the settings and the plates on every app update. With the
            # prompt the checkbox starts unticked, so an app-only or
            # plates-only flash leaves the rest alone unless someone ticks it.
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32-S3", "parts": chosen}],
        }

    def size_of(parts: list[dict]) -> int:
        return sum((dist / p["path"]).stat().st_size for p in parts)

    fonts = FIRMWARE / ".pio" / "webflash-data" / "fonts"
    if regions:
        shutil.rmtree(fonts, ignore_errors=True)
        fonts_into(fonts)

    builds: dict = {"version": version, "boards": {}}
    for board in boards:
        spec = BOARDS[board]
        pio(spec["env"])
        out = dist / board
        out.mkdir(exist_ok=True)
        app = app_parts(FIRMWARE / ".pio" / "build" / spec["env"], out)
        entry: dict = {
            "label": spec["label"],
            "app": {"manifest": f"manifest-{board}-app.json", "size": size_of(app)},
            "images": {},
        }
        manifests = {entry["app"]["manifest"]: manifest(f"{spec['label']}, app only", app)}
        if regions:
            packs = {r: pack_for(board, REGIONS[r][1], bake) for r in regions}
            # One image per region, or one image with them all - or, on a
            # board that offers it, both: the shared image and each region
            # alone at the larger size the whole partition allows.
            groups: dict[str, dict[str, Path]] = (
                {r: {r: p} for r, p in packs.items()}
                if spec["packs_per_image"] == 1
                else {"all": packs}
            )
            alone = spec.get("alone")
            if alone:
                for r in regions:
                    groups[r] = {r: pack_for(alone["key"], REGIONS[r][1], bake)}
            for key, group in groups.items():
                image = f"plates-{key}.bin"
                filesystem_image(board, group, out / image, fonts)
                part = [{"path": f"{board}/{image}", "offset": plates_offset(board)}]
                if key == "all":
                    label = "every region"
                elif alone:
                    label = REGIONS[key][0] + " alone, larger"
                else:
                    label = ", ".join(REGIONS[r][0] for r in group)
                manifests[f"manifest-{board}-{key}.json"] = manifest(
                    f"{spec['label']}, app and {label} plates", app + part
                )
                manifests[f"manifest-{board}-plates-{key}.json"] = manifest(
                    f"{spec['label']}, {label} plates only", part
                )
                entry["images"][key] = {
                    "label": label,
                    "regions": list(group),
                    "full": f"manifest-{board}-{key}.json",
                    "plates": f"manifest-{board}-plates-{key}.json",
                    "size": size_of(part),
                    "source": min(pack_source(p) for p in group.values()),
                }
        if regions and board == boards[0]:
            # The full-size plates, one file a species, once for every board.
            sys.path.insert(0, str(FIRMWARE / "tools"))
            from export_web_plates import export as export_plates

            for r in regions:
                card_pack = PACKS / "card" / f"{REGIONS[r][1]}.bin"
                if card_pack.exists():
                    n = export_plates(card_pack, dist / "plates" / r)
                    print(f"  web plates {r}: {n} species")
        if regions and spec.get("card"):
            # The card's packs are plain downloads, named as the firmware
            # looks for them at the card's root.
            card = dist / "card"
            card.mkdir(exist_ok=True)
            entry["card"] = {}
            for r in regions:
                src = pack_for("card", REGIONS[r][1], bake)
                name = f"plates-{r}.bin"
                shutil.copy(src, card / name)
                entry["card"][r] = {
                    "label": REGIONS[r][0],
                    "path": f"card/{name}",
                    "size": (card / name).stat().st_size,
                    "source": pack_source(card / name),
                }
        for name, m in manifests.items():
            (dist / name).write_text(json.dumps(m, indent=2) + "\n")
        builds["boards"][board] = entry
        total = sum(v["size"] for v in entry["images"].values()) + entry["app"]["size"]
        print(f"  {board}: {len(entry['images'])} plate image(s), {total / 1048576:.1f} MB")

    # manifest.json is what an old link to a full install gets: the first
    # board's first full image, or its app alone.
    first = builds["boards"][boards[0]]
    default = next(iter(first["images"].values()), None)
    (dist / "manifest.json").write_text(
        (dist / (default["full"] if default else first["app"]["manifest"])).read_text()
    )
    (dist / "builds.json").write_text(json.dumps(builds, indent=2) + "\n")
    shutil.copy(FIRMWARE / "webflash" / "index.html", dist / "index.html")
    print(f"assembled {dist}: version {version}")


def serve(dist: Path, port: int) -> None:
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(dist), **kw)

        def end_headers(self):
            # The manifest changes on every build; never let the browser cache it.
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    with http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler) as srv:
        print(f"open http://localhost:{port}/ in Chrome or Edge  (ctrl-c to stop)")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--no-serve", action="store_true", help="assemble dist/ and exit")
    ap.add_argument(
        "--boards",
        default=",".join(BOARDS),
        help="boards to build, comma-separated from " + ", ".join(BOARDS),
    )
    ap.add_argument(
        "--regions",
        default=",".join(REGIONS),
        help="plate packs to offer, comma-separated from "
        + ", ".join(REGIONS)
        + "; empty for the app alone",
    )
    ap.add_argument(
        "--app-only", action="store_true", help="leave the plates partition alone; app images only"
    )
    ap.add_argument(
        "--bake", action="store_true", help="bake any pack that is missing (minutes each)"
    )
    args = ap.parse_args()

    boards = [b for b in args.boards.split(",") if b]
    regions = [] if args.app_only else [r for r in args.regions.split(",") if r]
    for board in boards:
        if board not in BOARDS:
            raise SystemExit(f"no such board {board!r}; one of {', '.join(BOARDS)}")
    for region in regions:
        if region not in REGIONS:
            raise SystemExit(f"no such region {region!r}; one of {', '.join(REGIONS)}")
    if not boards:
        raise SystemExit("no boards to build")
    dist = FIRMWARE / "webflash" / "dist"
    assemble(dist, boards, regions, args.bake)
    if not args.no_serve:
        serve(dist, args.port)


if __name__ == "__main__":
    main()
