#!/usr/bin/env python3
"""Importer safety and binary preservation without copyrighted fixtures."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('native_import', Path(__file__).resolve().parents[1] / 'tools/import-linux.py')
imp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imp)


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'data'
        self.game = self.base / 'game'
        self.game.mkdir()
        self.exe = b'fake supported executable\x00\x1a\r\n'
        (self.game / 'game.exe').write_bytes(self.exe)
        (self.game / 'assets.r2').write_bytes(bytes(range(256)) * 1000)
        self.meta = {'testgame': {'name': 'Test Game', 'exe': 'game.exe',
                     'sha256': hashlib.sha256(self.exe).hexdigest(), 'assets': ['assets.r2']}}
        self.registry = patch.object(imp, 'TITLES', self.meta)
        self.registry.start()

    def tearDown(self):
        self.registry.stop()
        self.temp.cleanup()

    def test_zip_preserves_binary_and_nested_folder(self):
        archive = self.base / 'game.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for path in self.game.iterdir():
                z.write(path, 'nested/' + path.name.upper())
        imp.install(archive, self.root)
        self.assertEqual((self.root / 'testgame/GAME.EXE').read_bytes(), self.exe)
        self.assertEqual((self.root / 'testgame/ASSETS.R2').read_bytes(), (self.game / 'assets.r2').read_bytes())
        self.assertFalse(list(self.root.glob('.vct-import-*')))

    @unittest.skipUnless(shutil.which('7z'), '7zip not installed')
    def test_7zip_game_archive(self):
        archive = self.base / 'game.7z'
        subprocess.run(['7z', 'a', str(archive), '.'], cwd=self.game, stdout=subprocess.DEVNULL, check=True)
        imp.install(archive, self.root)
        self.assertEqual((self.root / 'testgame/game.exe').read_bytes(), self.exe)

    def test_missing_asset_never_installs(self):
        (self.game / 'assets.r2').unlink()
        with self.assertRaisesRegex(imp.ImportFailure, 'missing assets.r2'):
            imp.install(self.game, self.root)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_game_assets_across_disk_volumes(self):
        disk = self.base / 'volumes'
        (disk / 'system').mkdir(parents=True)
        (disk / 'assets').mkdir()
        (self.game / 'game.exe').rename(disk / 'system/game.exe')
        (self.game / 'assets.r2').rename(disk / 'assets/assets.r2')
        (disk / 'system/unrelated.txt').write_text('not game data')
        imp.install(disk, self.root)
        self.assertEqual((self.root / 'testgame/game.exe').read_bytes(), self.exe)
        self.assertTrue((self.root / 'testgame/assets.r2').is_file())
        self.assertFalse((self.root / 'testgame/unrelated.txt').exists())

    def test_existing_install_and_saves_preserved(self):
        imp.install(self.game, self.root)
        marker = self.root / 'testgame/keep'
        marker.write_text('existing')
        with self.assertRaisesRegex(imp.ImportFailure, 'already exists'):
            imp.install(self.game, self.root)
        self.assertEqual(marker.read_text(), 'existing')

    def test_rejects_traversal_and_symlinks(self):
        for name, mode in [('../escape', 0), ('/escape', 0), ('link', 0o120777 << 16)]:
            archive = self.base / 'bad.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                entry = zipfile.ZipInfo(name)
                entry.external_attr = mode
                z.writestr(entry, 'payload')
            with self.assertRaisesRegex(imp.ImportFailure, 'unsafe'):
                imp.install(archive, self.root)
        self.assertFalse((self.base / 'escape').exists())

    def test_bios_only_archive_explains_disk_requirement(self):
        archive = self.base / 'bios.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('bios.bin', b'firmware')
        with self.assertRaisesRegex(imp.ImportFailure, 'BIOS-only'):
            imp.install(archive, self.root)

    def test_primary_and_logical_partitions(self):
        disk = bytearray(512 * 200)
        def entry(sector, index, kind, start, count):
            disk[sector * 512 + 510:sector * 512 + 512] = b'\x55\xaa'
            off = sector * 512 + 446 + index * 16
            disk[off + 4] = kind
            struct.pack_into('<II', disk, off + 8, start, count)
        entry(0, 0, 6, 1, 9)
        entry(0, 1, 5, 20, 150)
        entry(20, 0, 6, 1, 10)
        entry(20, 1, 5, 30, 50)
        entry(50, 0, 6, 1, 10)
        entry(50, 1, 5, 30, 50)  # cyclic EBR must terminate
        path = self.base / 'disk.img'
        path.write_bytes(disk)
        self.assertEqual(imp.partitions(path), [(512, 4608), (10752, 5120), (26112, 5120)])


if __name__ == '__main__':
    unittest.main()
