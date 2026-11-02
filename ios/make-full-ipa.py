#!/usr/bin/env python3
"""Adds a Castlevania Fighter game folder to an unsigned CvF Online IPA.

Usage: make-full-ipa.py <app.ipa> <game folder> <config.ini> <output.ipa>

The result contains the game, so keep it private: never upload it to this repo
or its releases. The engine scripts come from the IPA so they match its engine.
"""
import copy
import os
import shutil
import sys
import zipfile

GAME_DIRS = ("chars", "data", "external", "font", "sound", "stages")
SKIP_NAMES = {".DS_Store", "Thumbs.db"}
SKIP_EXTENSIONS = {".exe", ".dll", ".so", ".dylib"}


def add_file(out, source, name):
    info = zipfile.ZipInfo.from_file(source, name)
    info.external_attr = 0o100644 << 16
    info.compress_type = zipfile.ZIP_STORED  # Sideloadly unpacks it anyway, and this is much faster
    with open(source, "rb") as src, out.open(info, "w", force_zip64=True) as dst:
        while chunk := src.read(1 << 20):
            dst.write(chunk)


def main(ipa, game, config, output):
    for need in ("chars", "data/options11.def", "stages", "sound", "font"):
        if not os.path.exists(os.path.join(game, need)):
            sys.exit(f"{game} is missing {need}")

    with zipfile.ZipFile(ipa) as src, zipfile.ZipFile(output + ".tmp", "w", zipfile.ZIP_DEFLATED, allowZip64=True) as out:
        apps = {n.split("/")[1] for n in src.namelist() if n.startswith("Payload/") and n.split("/")[1].endswith(".app")}
        if len(apps) != 1:
            sys.exit(f"expected one app in the IPA, found {apps}")
        app = f"Payload/{apps.pop()}/"

        for info in src.infolist():
            data = src.read(info)
            out.writestr(copy.copy(info), data, compress_type=info.compress_type)

        # Engine-matched scripts and controller DB, from the app's DefaultAssets.zip.
        src.extract(app + "DefaultAssets.zip", output + ".assets")
        assets = zipfile.ZipFile(os.path.join(output + ".assets", app + "DefaultAssets.zip"))
        replaced = set()
        for info in assets.infolist():
            if info.is_dir() or info.filename == "manifest.txt":
                continue
            replaced.add(info.filename)
            target = zipfile.ZipInfo(f"{app}Game/{info.filename}", info.date_time)
            target.external_attr = 0o100644 << 16
            target.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(target, assets.read(info))
        if "external/script/main.lua" not in replaced:
            sys.exit("the IPA has no engine scripts")

        count = 0
        for top in GAME_DIRS:
            for folder, dirs, files in os.walk(os.path.join(game, top)):
                dirs.sort()
                for file in sorted(files):
                    source = os.path.join(folder, file)
                    rel = os.path.relpath(source, game).replace(os.sep, "/")
                    if file in SKIP_NAMES or os.path.splitext(file)[1].lower() in SKIP_EXTENSIONS or rel in replaced:
                        continue
                    add_file(out, source, f"{app}Game/{rel}")
                    count += 1
                    if count % 500 == 0:
                        print(count, "files", flush=True)
        add_file(out, config, f"{app}Game/save/config.ini")

    shutil.rmtree(output + ".assets", ignore_errors=True)
    os.replace(output + ".tmp", output)
    print("DONE", count, "files", os.path.getsize(output), "bytes", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
