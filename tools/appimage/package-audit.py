#!/usr/bin/env python3
"""Collect/check Debian-built AppImage notices and exact executable identity.

Never launches the application. Unknown ELF provenance or missing notices fail closed.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import quote

LICENSE_DIR = Path('usr/share/licenses/rostrum')
RUNTIME_DIR = Path(__file__).resolve().parent / 'licenses/runtime'
RUNTIME_NOTICES = tuple(json.loads((RUNTIME_DIR / 'sources.json').read_text())['notices'])

SENTRY_NOTICES = ('Sentry-LICENSE', 'Sentry-mpack-LICENSE', 'Sentry-jsmn-LICENSE',
                  'Sentry-stb-LICENSE', 'Sentry-libunwind-COPYING')


def run(*args):
    return subprocess.check_output(args, text=True, timeout=60).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_id(path):
    match = re.search(r'Build ID: ([0-9a-f]+)', run('readelf', '-n', str(path)))
    if not match:
        raise ValueError(f'ELF build ID missing: {path}')
    return match[1]


def text_hash(path):
    with tempfile.TemporaryDirectory(prefix='rostrum-text-') as temp:
        output = Path(temp) / 'text'
        # objcopy defaults to rewriting its input when no output is supplied.
        # Use a disposable ELF copy so AppImage trailing SquashFS bytes stay intact.
        run('objcopy', '--dump-section', f'.text={output}', str(path), str(Path(temp) / 'elf-copy'))
        if not output.is_file() or not output.stat().st_size:
            raise ValueError(f'ELF .text missing: {path}')
        return digest(output)


def executable_identity(original, packaged):
    if '.debug_info' not in run('readelf', '-S', str(original)):
        raise ValueError('Retained executable has no .debug_info')
    first = {'build_id': build_id(original), 'text_sha256': text_hash(original)}
    second = {'build_id': build_id(packaged), 'text_sha256': text_hash(packaged)}
    if first != second:
        raise ValueError('Unstripped/packaged executable identity mismatch')
    return dict(first, unstripped_sha256=digest(original), packaged_sha256=digest(packaged))


def elf_files(root):
    for path in sorted((root / 'usr').rglob('*')):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    yield path


def required_common(root):
    required = {'GPL-2'}  # libfuse LGPLv2.1 incorporates GPLv2.
    for path in (root / 'usr/share/doc').glob('*/copyright'):
        required.update(name.rstrip('.') for name in re.findall(r'/usr/share/common-licenses/([\w.+-]+)', path.read_text(errors='replace')))
    # LGPLv3 incorporates GPLv3; LGPLv2/2.1 incorporates GPLv2.
    for name in tuple(required):
        if name.startswith('LGPL'):
            required.add('GPL-3' if name in ('LGPL-3', 'LGPL') else 'GPL-2')
    return required


def check_material(root, sentry):
    own_license = root / 'usr/share/doc/rostrum/LICENSE'
    if not own_license.is_file() or not own_license.read_bytes().strip():
        raise ValueError('Missing Rostrum LICENSE')
    names = ['RNNoise-COPYING', 'Breeze-copyright']
    if sentry:
        names.extend(SENTRY_NOTICES)
    names.extend(RUNTIME_NOTICES)
    names.append('DEPENDENCIES.md')
    for name in names:
        path = root / LICENSE_DIR / name
        if not path.is_file() or not path.read_bytes().strip():
            raise ValueError(f'Missing/empty bundled notice: {name}')
    for name in sorted(required_common(root)):
        path = root / 'usr/share/common-licenses' / name
        if not path.is_file() or not path.read_bytes().strip():
            raise ValueError(f'Missing referenced license text: {name}')


def package_info(package):
    fields = run('dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}', package).split('\t')
    binary, version, source, source_version = fields
    return {'binary': binary, 'version': version, 'source': source, 'source_version': source_version,
            'source_url': f'https://launchpad.net/ubuntu/+source/{quote(source, safe="")}/{quote(source_version, safe="")}'}


def owner(path):
    # linuxdeploy preserves the source ELF build ID through RPATH/strip edits.
    # A basename alone is insufficient when multiple versions are installed.
    result = subprocess.run(['dpkg-query', '-S', '*/' + path.name], text=True, capture_output=True, timeout=60)
    wanted = build_id(path)
    for line in result.stdout.splitlines():
        if ': /' not in line:
            continue
        package, original = line.split(': /', 1)
        original = Path('/' + original)
        if original.is_file() and build_id(original) == wanted:
            return package, wanted
    raise ValueError(f'No matching installed-package ELF/build ID: {path}')


def project_build_path(relative):
    paths = {'usr/bin/rostrum': 'src/app/rostrum',
             'usr/bin/rostrum-probe': 'tools/rostrum-probe/rostrum-probe'}
    if relative.endswith('/rostrum/librostrum-dsp.so'):
        return 'lib/rostrum/librostrum-dsp.so'
    return paths.get(relative)


def cache_value(build, key):
    match = re.search(r'^' + key + r':[^=]*=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M)
    if not match:
        raise ValueError(f'Missing CMake cache setting: {key}')
    return match[1]


def fetched_source(build, name):
    value = cache_value(build, 'FETCHCONTENT_SOURCE_DIR_' + name.upper())
    return Path(value) if value else build / '_deps' / (name + '-src')


def collect(root, build):
    notices = root / LICENSE_DIR
    notices.mkdir(parents=True, exist_ok=True)
    sentry = cache_value(build, 'ROSTRUM_WITH_SENTRY') == 'ON'
    if sentry:
        source = fetched_source(build, 'sentry')
        shutil.copyfile(source / 'LICENSE', notices / 'Sentry-LICENSE')
        for component in ('mpack', 'jsmn'):
            text = (source / 'vendor' / (component + '.h')).read_text()
            # Copy the complete leading upstream license comment, without rewriting it.
            comment = text[:text.index('*/') + 2]
            if 'Permission is hereby granted' not in comment:
                raise ValueError(f'Upstream {component} license layout changed')
            (notices / f'Sentry-{component}-LICENSE').write_text(comment + '\n')
        text = (source / 'vendor/stb_sprintf.h').read_text()
        license_text = text[text.rindex('/*'):]
        if 'ALTERNATIVE A - MIT License' not in license_text:
            raise ValueError('Upstream stb license layout changed')
        (notices / 'Sentry-stb-LICENSE').write_text(license_text)
        shutil.copyfile(source / 'vendor/libunwind/COPYING', notices / 'Sentry-libunwind-COPYING')
    runtime = json.loads((RUNTIME_DIR / 'sources.json').read_text())
    for name, record in runtime['notices'].items():
        source = RUNTIME_DIR / name
        if digest(source) != record['sha256']:
            raise ValueError(f'Pinned upstream runtime notice changed: {name}')
        shutil.copyfile(source, notices / name)
    packages = {}
    files = {}
    projects = {}
    for path in elf_files(root):
        relative = str(path.relative_to(root))
        own = project_build_path(relative)
        if own:
            identity = build_id(path)
            if identity != build_id(build / own):
                raise ValueError(f'Project ELF does not match build: {relative}')
            projects[relative] = identity
            continue
        package, identity = owner(path)
        record = package_info(package)
        packages[record['binary']] = record
        files[relative] = {'package': record['binary'], 'build_id': identity}
    # Resources already collected by linuxdeploy, plus icons and headers compiled
    # into Rostrum: retain their versioned source-package provenance as well.
    for doc in (root / 'usr/share/doc').glob('*/copyright'):
        if doc.parent.name != 'rostrum':
            record = package_info(doc.parent.name)
            packages.setdefault(record['binary'], record)
    for package in ('breeze-icon-theme', 'libtomlplusplus-dev'):
        record = package_info(package)
        packages[record['binary']] = record
    for package in packages:
        source = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
        if not source.is_file():
            raise ValueError(f'Package copyright unavailable: {package}')
        target = root / 'usr/share/doc' / package.split(':')[0] / 'copyright'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for name in required_common(root):
        source = Path('/usr/share/common-licenses') / name
        if not source.is_file():
            raise ValueError(f'Builder lacks referenced license: {name}')
        target = root / 'usr/share/common-licenses' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    doc = Path(__file__).resolve().parents[2] / 'docs/appimage-dependencies.md'
    shutil.copyfile(doc, notices / 'DEPENDENCIES.md')
    manifest = {'runtime': runtime, 'sentry': sentry, 'packages': packages, 'elf_files': files, 'project_elf_files': projects,
                'static_sources': {'rnnoise': 'https://github.com/xiph/rnnoise/releases/tag/v0.2'}}
    if sentry:
        manifest['static_sources']['sentry'] = 'https://github.com/getsentry/sentry-native/releases/tag/0.17.1'
    (notices / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    manifest['notice_sha256'] = {str(p.relative_to(root)): digest(p) for p in sorted(root.rglob('*'))
                                if p.is_file() and (p.name in ('copyright', 'LICENSE') or p.parent == notices or 'common-licenses' in p.parts)
                                and p.name != 'manifest.json'}
    (notices / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    check(root)


def check(root):
    path = root / LICENSE_DIR / 'manifest.json'
    if not path.is_file():
        raise ValueError('Missing license manifest (legacy incomplete package)')
    manifest = json.loads(path.read_text())
    check_material(root, manifest['sentry'])
    for relative, expected in manifest['notice_sha256'].items():
        path = root / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Notice missing/changed: {relative}')
    observed = set()
    project_observed = set()
    for path in elf_files(root):
        relative = str(path.relative_to(root))
        if project_build_path(relative):
            project_observed.add(relative)
            if build_id(path) != manifest['project_elf_files'].get(relative):
                raise ValueError(f'Unaccounted/mismatched project ELF: {relative}')
            continue
        observed.add(relative)
        record = manifest['elf_files'].get(relative)
        if not record or build_id(path) != record['build_id'] or record['package'] not in manifest['packages']:
            raise ValueError(f'Unaccounted/mismatched bundled ELF: {relative}')
    if project_observed != set(manifest['project_elf_files']):
        raise ValueError('Project ELF inventory changed')
    if observed != set(manifest['elf_files']):
        raise ValueError('Bundled ELF inventory changed')
    print(f'License audit passed: {len(observed)} dependency ELFs, {len(manifest["packages"])} packages')


def check_runtime(appimage):
    expected = json.loads((RUNTIME_DIR / 'sources.json').read_text())
    if build_id(appimage) != expected['build_id'] or text_hash(appimage) != expected['text_sha256']:
        raise ValueError('Packaged launcher differs from license-matched pinned runtime')
    print('Pinned launcher build ID and code hash match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('collect', 'check', 'symbols'))
    parser.add_argument('root', type=Path, help='Extracted AppDir')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--appimage', type=Path)
    parser.add_argument('--symbols', type=Path)
    parser.add_argument('--source-commit')
    args = parser.parse_args()
    if args.action == 'collect':
        if not args.build:
            parser.error('collect requires --build')
        collect(args.root, args.build)
    elif args.action == 'check':
        check(args.root)
        if args.appimage:
            check_runtime(args.appimage)
    else:
        if not args.build or not args.symbols or not args.source_commit:
            parser.error('symbols requires --build, --symbols, --source-commit')
        check(args.root)
        original = args.build / 'src/app/rostrum'
        identity = executable_identity(original, args.root / 'usr/bin/rostrum')
        args.symbols.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, args.symbols / 'rostrum')
        # Standalone binary downloads must retain their accompanying notices.
        for directory in ('licenses', 'common-licenses', 'doc'):
            shutil.copytree(args.root / 'usr/share' / directory,
                            args.symbols / 'usr/share' / directory, dirs_exist_ok=True)
        identity['source_commit'] = args.source_commit
        (args.symbols / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
        print(json.dumps(identity, indent=2))


if __name__ == '__main__':
    main()
