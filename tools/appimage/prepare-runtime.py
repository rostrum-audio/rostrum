#!/usr/bin/env python3
"""Unpack the AppImage runtime and check the recorded checksum."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import tempfile


def prepare_runtime(archive, expected_sha, destination):
    data = gzip.decompress(archive.read_bytes())
    if hashlib.sha256(data).hexdigest() != expected_sha:
        raise ValueError('AppImage runtime checksum does not match its licence record')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
        temporary.chmod(0o755)
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    try:
        identity = json.loads((here / 'licenses/runtime/sources.json').read_text())
        prepare_runtime(here / 'runtime/runtime-x86_64.gz', identity['sha256'], args.destination)
    except (OSError, ValueError, EOFError) as error:
        parser.exit(1, f'{error}\n')
