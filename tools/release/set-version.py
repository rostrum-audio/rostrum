#!/usr/bin/env python3
"""Sets the release version in CMakeLists.txt and the AppStream metainfo, as docs/releasing.md asks.

  set-version.py --version 0.2.0[-rc1] --notes "One or two sentences." [--date YYYY-MM-DD] [--root DIR]

The CMake version becomes the version without its suffix. The metainfo gets a <release> entry for
that version at the top of <releases>, described by the notes, unless it already has one; a final
release (no suffix) moves an existing entry's date to the release date. Prints "changed" or
"unchanged". Fails if the version would go backwards.
"""

import argparse
import datetime
import html
import pathlib
import re
import sys

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.]+)?$")
CMAKE_RE = re.compile(r"^(project\(rostrum VERSION )(\d+\.\d+\.\d+)", re.MULTILINE)
METAINFO = "data/dev.getrostrum.Rostrum.metainfo.xml"


def parts(version):
    return tuple(int(x) for x in version.split("."))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--version", required=True)
    p.add_argument("--notes", required=True)
    p.add_argument("--date", default=datetime.date.today().isoformat())
    p.add_argument("--root", default=".")
    args = p.parse_args()

    m = VERSION_RE.match(args.version)
    if not m:
        sys.exit(f"'{args.version}' is not a version like 0.2.0 or 0.2.0-rc1.")
    base = args.version.split("-", 1)[0]
    final = m.group(4) is None
    notes = " ".join(args.notes.split())
    if not notes:
        sys.exit("The release notes are empty.")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", args.date):
        sys.exit(f"'{args.date}' is not a date like 2026-10-03.")

    root = pathlib.Path(args.root)
    cmake_path = root / "CMakeLists.txt"
    cmake = cmake_path.read_text()
    found = CMAKE_RE.search(cmake)
    if not found:
        sys.exit("CMakeLists.txt has no 'project(rostrum VERSION x.y.z' line.")
    current = found.group(2)
    if parts(base) < parts(current):
        sys.exit(f"{base} is older than the current version {current}.")
    new_cmake = CMAKE_RE.sub(lambda c: c.group(1) + base, cmake, count=1)

    meta_path = root / METAINFO
    meta = meta_path.read_text()
    entry = re.compile(r'(<release version="' + re.escape(base) + r'" date=")([^"]*)(")')
    if entry.search(meta):
        new_meta = entry.sub(lambda e: e.group(1) + args.date + e.group(3), meta, count=1) if final else meta
    else:
        opening = re.search(r"^([ \t]*)<releases>\n", meta, re.MULTILINE)
        if not opening:
            sys.exit(f"{METAINFO} has no <releases> element.")
        indent = opening.group(1)
        step = "  "
        block = (
            f'{indent}{step}<release version="{base}" date="{args.date}">\n'
            f"{indent}{step * 2}<description>\n"
            f"{indent}{step * 3}<p>{html.escape(notes, quote=False)}</p>\n"
            f"{indent}{step * 2}</description>\n"
            f"{indent}{step}</release>\n"
        )
        new_meta = meta[: opening.end()] + block + meta[opening.end():]

    changed = new_cmake != cmake or new_meta != meta
    if changed:
        cmake_path.write_text(new_cmake)
        meta_path.write_text(new_meta)
    print("changed" if changed else "unchanged")


if __name__ == "__main__":
    main()
