#!/usr/bin/env python3
"""Writes the latest.json update feed for one release, in the format src/core/UpdateFeed.cpp reads.

  make-feed.py --version 0.2.0 --page URL --download-base URL [--notes-file F] [--date YYYY-MM-DD]
               AppImage [AppImage ...] > latest.json

Each AppImage must be named Rostrum-<version>-<arch>.AppImage; its URL is <download-base>/<name>.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", required=True)
    p.add_argument("--page", required=True, help="release page shown as What's New")
    p.add_argument("--download-base", required=True, help="URL the AppImages are downloaded from")
    p.add_argument("--notes-file", help="plain-text release notes")
    p.add_argument("--date", default=datetime.date.today().isoformat())
    p.add_argument("appimages", nargs="+")
    args = p.parse_args()

    # The updater ignores versions with a suffix, so a feed for one would never be offered.
    if not re.fullmatch(r"\d+(\.\d+){0,3}", args.version):
        sys.exit(f"version {args.version!r} is not a plain release number")
    datetime.date.fromisoformat(args.date)
    for url in (args.page, args.download_base):
        if not url.startswith("https://"):
            sys.exit(f"{url} is not an https:// URL; the updater would drop it")

    notes = ""
    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as f:
            notes = f.read().strip()

    appimage = {}
    for path in args.appimages:
        name = os.path.basename(path)
        m = re.fullmatch(rf"Rostrum-{re.escape(args.version)}-([A-Za-z0-9_]+)\.AppImage", name)
        if not m:
            sys.exit(f"{name} is not named Rostrum-{args.version}-<arch>.AppImage")
        appimage[m.group(1)] = {
            "url": f"{args.download_base.rstrip('/')}/{name}",
            "sha256": sha256(path),
            "size": os.path.getsize(path),
        }

    feed = {
        "version": args.version,
        "date": args.date,
        "notes": notes,
        "url": args.page,
        "appimage": appimage,
    }
    json.dump(feed, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
