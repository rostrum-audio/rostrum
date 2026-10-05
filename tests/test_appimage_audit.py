"""Offline packaging regressions; no application or audio daemon is launched."""
import importlib.util
import json
from pathlib import Path
import tempfile
import shutil
import subprocess
import unittest

spec = importlib.util.spec_from_file_location('audit', Path(__file__).resolve().parents[1] / 'tools/appimage/package-audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class LicenseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.licenses = self.root / 'usr/share/licenses/rostrum'
        self.licenses.mkdir(parents=True)
        for name in ('Sentry-LICENSE', 'Sentry-mpack-LICENSE', 'Sentry-jsmn-LICENSE', 'Sentry-stb-LICENSE', 'Sentry-libunwind-COPYING', 'RNNoise-COPYING', 'Breeze-copyright', 'DEPENDENCIES.md'):
            (self.licenses / name).write_text('notice\n')
        for name in ('RNNoise-components-NOTICES', 'Sentry-libunwind-components-NOTICES'):
            (self.licenses / name).write_text('component notices\n')
        for name in audit.RUNTIME_NOTICES:
            (self.licenses / name).write_text('notice\n')
        doc = self.root / 'usr/share/doc/qt/copyright'
        doc.parent.mkdir(parents=True)
        doc.write_text('License: LGPL-3\n See /usr/share/common-licenses/LGPL-3.\n')
        own = self.root / 'usr/share/doc/rostrum/LICENSE'
        own.parent.mkdir(parents=True)
        own.write_text('Apache license\n')
        self.common = self.root / 'usr/share/common-licenses'
        self.common.mkdir(parents=True)
        (self.common / 'LGPL-3').write_text('license text\n')
        (self.common / 'GPL-3').write_text('license text\n')
        (self.common / 'GPL-2').write_text('license text\n')

    def test_missing_component_notices_rejected(self):
        for name in ('RNNoise-components-NOTICES', 'Sentry-libunwind-components-NOTICES'):
            with self.subTest(name=name):
                path = self.licenses / name
                if path.exists():
                    path.unlink()
                with self.assertRaisesRegex(ValueError, name):
                    audit.check_material(self.root, sentry=True)
                path.write_text('component notices\n')

    def test_component_headers_preserve_separate_copyright_and_terms(self):
        source = self.root / 'upstream'
        (source / 'src').mkdir(parents=True)
        header = '/* Copyright CSIRO */\n/** file description */\n/* Redistribution and use permitted */\n'
        (source / 'src/pitch.c').write_text(header + '#include "pitch.h"\n')
        text = audit.component_notices(source)
        self.assertIn(header, text)
        self.assertIn('src/pitch.c', text)
        self.assertNotIn('#include', text)

    def test_complete_material_passes(self):
        audit.check_material(self.root, sentry=True)

    def test_missing_sentry_notice_rejected(self):
        (self.licenses / 'Sentry-LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'Sentry-LICENSE'):
            audit.check_material(self.root, sentry=True)

    def test_missing_vendor_notice_rejected(self):
        (self.licenses / 'Sentry-mpack-LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'Sentry-mpack'):
            audit.check_material(self.root, sentry=True)

    def test_missing_referenced_license_rejected(self):
        (self.common / 'LGPL-3').unlink()
        with self.assertRaisesRegex(ValueError, 'LGPL-3'):
            audit.check_material(self.root, sentry=True)

    def test_lgpl_requires_gpl_text_too(self):
        (self.common / 'GPL-3').unlink()
        with self.assertRaisesRegex(ValueError, 'GPL-3'):
            audit.check_material(self.root, sentry=True)

    def test_missing_launcher_notice_rejected(self):
        (self.licenses / 'Runtime-mimalloc-LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'Runtime-mimalloc'):
            audit.check_material(self.root, sentry=True)

    def test_empty_notice_rejected(self):
        (self.licenses / 'Sentry-LICENSE').write_text('')
        with self.assertRaisesRegex(ValueError, 'Sentry-LICENSE'):
            audit.check_material(self.root, sentry=True)


class SymbolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = self.root / 'main.c'
        source.write_text('int main(void) { return 0; }\n')
        self.original = self.root / 'original'
        self.packaged = self.root / 'packaged'
        subprocess.run(['cc', '-g', '-Wl,--build-id', str(source), '-o', str(self.original)], check=True)
        shutil.copyfile(self.original, self.packaged)
        subprocess.run(['strip', str(self.packaged)], check=True)

    def test_text_inspection_preserves_trailing_artifact_bytes(self):
        with self.packaged.open('ab') as stream:
            stream.write(b'fake-squashfs-trailing-data')
        before = self.packaged.read_bytes()
        audit.text_hash(self.packaged)
        self.assertEqual(before, self.packaged.read_bytes())

    def test_strip_keeps_exact_identity(self):
        result = audit.executable_identity(self.original, self.packaged)
        self.assertNotEqual(result['unstripped_sha256'], result['packaged_sha256'])
        self.assertEqual(result['build_id'], audit.build_id(self.packaged))

    def test_rebuild_identity_rejected(self):
        source = self.root / 'other.c'
        source.write_text('int main(void) { return 7; }\n')
        subprocess.run(['cc', '-g', '-Wl,--build-id', str(source), '-o', str(self.packaged)], check=True)
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            audit.executable_identity(self.original, self.packaged)

    def test_same_build_id_altered_code_rejected(self):
        section = self.root / 'text'
        subprocess.run(['objcopy', '--dump-section', '.text=' + str(section), str(self.packaged)], check=True)
        code = bytearray(section.read_bytes())
        code[0] ^= 1
        section.write_bytes(code)
        subprocess.run(['objcopy', '--update-section', '.text=' + str(section), str(self.packaged)], check=True)
        self.assertEqual(audit.build_id(self.original), audit.build_id(self.packaged))
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            audit.executable_identity(self.original, self.packaged)

    def project_fixture(self):
        fixture = LicenseTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        target = fixture.root / 'usr/bin/rostrum-probe'
        target.parent.mkdir(parents=True)
        shutil.copyfile(self.packaged, target)
        manifest = {'sentry': True, 'notice_sha256': {}, 'packages': {},
                    'elf_files': {}, 'project_elf_files': {
                        'usr/bin/rostrum-probe': audit.build_id(self.packaged)}}
        (fixture.licenses / 'manifest.json').write_text(json.dumps(manifest))
        return fixture, target

    def test_installed_probe_accounted_by_build_identity(self):
        fixture, target = self.project_fixture()
        audit.check(fixture.root)

    def test_probe_identity_mismatch_rejected(self):
        fixture, target = self.project_fixture()
        manifest_file = fixture.licenses / 'manifest.json'
        manifest = json.loads(manifest_file.read_text())
        manifest['project_elf_files']['usr/bin/rostrum-probe'] = 'not-this-build'
        manifest_file.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'mismatched project ELF'):
            audit.check(fixture.root)

    def test_unknown_executable_not_exempted(self):
        fixture, target = self.project_fixture()
        shutil.copyfile(target, target.parent / 'unaccounted')
        with self.assertRaisesRegex(ValueError, 'Unaccounted/mismatched bundled ELF'):
            audit.check(fixture.root)

    def test_changed_notice_hash_rejected(self):
        fixture, target = self.project_fixture()
        manifest_file = fixture.licenses / 'manifest.json'
        manifest = json.loads(manifest_file.read_text())
        notice = fixture.licenses / 'Sentry-LICENSE'
        relative = str(notice.relative_to(fixture.root))
        manifest['notice_sha256'][relative] = audit.digest(notice)
        manifest_file.write_text(json.dumps(manifest))
        notice.write_text('altered notice')
        with self.assertRaisesRegex(ValueError, 'Notice missing/changed'):
            audit.check(fixture.root)

    def test_retained_symbols_include_matching_license_material(self):
        fixture, target = self.project_fixture()
        packaged = fixture.root / 'usr/bin/rostrum'
        shutil.copyfile(self.packaged, packaged)
        manifest_file = fixture.licenses / 'manifest.json'
        manifest = json.loads(manifest_file.read_text())
        manifest['project_elf_files']['usr/bin/rostrum'] = audit.build_id(packaged)
        manifest_file.write_text(json.dumps(manifest))
        build = self.root / 'build'
        original = build / 'src/app/rostrum'
        original.parent.mkdir(parents=True)
        shutil.copyfile(self.original, original)
        output = self.root / 'retained'
        subprocess.run(['python3', '-B', str(spec.origin), 'symbols', str(fixture.root),
                        '--build', str(build), '--symbols', str(output),
                        '--source-commit', 'test-source'], check=True, timeout=30,
                       capture_output=True)
        for relative in ('usr/share/licenses/rostrum/Sentry-LICENSE',
                         'usr/share/common-licenses/LGPL-3',
                         'usr/share/doc/rostrum/LICENSE',
                         'usr/share/licenses/rostrum/manifest.json'):
            self.assertEqual((fixture.root / relative).read_bytes(),
                             (output / relative).read_bytes())
        self.assertEqual(audit.digest(original), audit.digest(output / 'rostrum'))

    def test_unmatched_launcher_rejected(self):
        with self.assertRaisesRegex(ValueError, 'license-matched pinned runtime'):
            audit.check_runtime(self.packaged)

    def test_stripped_retention_rejected(self):
        with self.assertRaisesRegex(ValueError, 'no .debug_info'):
            audit.executable_identity(self.packaged, self.packaged)


if __name__ == '__main__':
    unittest.main()
