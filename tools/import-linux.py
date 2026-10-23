#!/usr/bin/env python3
"""Binary-safe, unprivileged native game importer. No game data is bundled."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

# Replaced by the build's embed-import.py when bundled into the native host.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from titles import TITLES


class ImportFailure(Exception):
    pass


def report(percent, message):
    print(f'PCT {percent} {message}', flush=True)


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
    return checksum.hexdigest()


def partitions(image):
    """Primary and logical MBR volumes, with bounded/cycle-checked EBR walks."""
    size = image.stat().st_size
    found = []
    with image.open('rb') as stream:
        def table(lba):
            stream.seek(lba * 512)
            sector = stream.read(512)
            if len(sector) != 512 or sector[510:] != b'\x55\xaa':
                return []
            return [(sector[p + 4], *struct.unpack_from('<II', sector, p + 8))
                    for p in range(446, 510, 16)]

        for kind, start, count in table(0):
            if kind in (0x05, 0x0f, 0x85):
                base, current, seen = start, start, set()
                while current not in seen and len(seen) < 128:
                    seen.add(current)
                    entries = table(current)
                    if not entries:
                        break
                    k, rel, n = entries[0]
                    if k and n and (current + rel + n) * 512 <= size:
                        found.append(((current + rel) * 512, n * 512))
                    k, rel, n = entries[1]
                    if k not in (0x05, 0x0f, 0x85) or not rel:
                        break
                    current = base + rel
            elif kind and count and (start + count) * 512 <= size:
                found.append((start * 512, count * 512))
    return found or [(0, size)]


def run(command, message):
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        detail = result.stdout.decode(errors='replace')[-1200:].strip()
        raise ImportFailure(f'{message}: {detail}')


def extract_chd(source, work):
    for tool in ('chdman', 'mcopy', '7z'):
        if not shutil.which(tool):
            raise ImportFailure(f'CHD import needs {tool}. Install chdman (MAME tools), mtools and 7zip.')
    image = work / 'disk.img'
    report(5, 'Extracting CHD disk; this can take a few minutes...')
    run(['chdman', 'extracthd', '-i', str(source), '-o', str(image)], 'Cannot extract CHD')
    roots = []
    volumes = partitions(image)
    for number, (offset, length) in enumerate(volumes):
        report(35 + number * 30 // len(volumes), f'Reading disk volume {number + 1}/{len(volumes)}...')
        target = work / f'volume-{number}'
        target.mkdir()
        # mcopy's -a/-t are intentionally absent: they corrupt binary CSH banks.
        result = subprocess.run(['mcopy', '-s', '-n', '-i', f'{image}@@{offset}',
                                 '::*', str(target)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            # Some cabinet FAT32 images are rejected by mtools but readable by 7zip.
            part = work / 'partition.img'
            with image.open('rb') as src, part.open('wb') as dst:
                src.seek(offset)
                remaining = length
                while remaining:
                    chunk = src.read(min(4 * 1024 * 1024, remaining))
                    if not chunk:
                        raise ImportFailure('Truncated disk partition')
                    dst.write(chunk)
                    remaining -= len(chunk)
            result = subprocess.run(['7z', 'x', '-y', '-bd', f'-o{target}', str(part)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            part.unlink()
        if result.returncode == 0:
            roots.append(target)
    if not roots:
        raise ImportFailure('No readable FAT game partition found in this CHD.')
    return roots


def safe_member(name):
    parts = name.replace('\\', '/').split('/')
    return bool(name) and not name.startswith(('/', '\\')) and not any(
        p == '..' or ':' in p for p in parts)


def extract_archive(source, work):
    target = work / 'archive'
    target.mkdir()
    report(10, 'Unpacking game files...')
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            for entry in archive.infolist():
                if not safe_member(entry.filename) or stat.S_ISLNK(entry.external_attr >> 16):
                    raise ImportFailure('Archive contains an unsafe path or symbolic link.')
                if not entry.is_dir():
                    destination = target.joinpath(*entry.filename.replace('\\', '/').split('/'))
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(entry) as src, destination.open('wb') as dst:
                        shutil.copyfileobj(src, dst)
    else:
        if not shutil.which('7z'):
            raise ImportFailure('This archive needs 7zip. ZIP files also work without it.')
        result = subprocess.run(['7z', 'l', '-slt', str(source)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        if result.returncode:
            raise ImportFailure('Cannot read this archive.')
        listing = result.stdout.partition('----------')[2]
        for line in listing.splitlines():
            if line.startswith('Path = ') and not safe_member(line[7:]):
                raise ImportFailure('Archive contains an unsafe path.')
            if line.startswith(('Symbolic Link = ', 'Hard Link = ')) or (
                    line.startswith('Attributes = ') and 'l' in line[13:]):
                raise ImportFailure('Archive contains a symbolic or hard link.')
        run(['7z', 'x', '-y', '-bd', f'-o{target}', str(source)], 'Cannot unpack archive')
    return [target]


def identify(roots):
    unsupported = False
    for root in roots:
        for path in root.rglob('*'):
            if path.is_symlink():
                continue
            for title, meta in TITLES.items():
                if path.name.casefold() != meta['exe'].casefold() or not path.is_file():
                    continue
                if digest(path) != meta['sha256']:
                    unsupported = True
                    continue
                # The cabinets split assets across DOS volumes. Combine their
                # game files without allowing another executable to take over.
                def game_file(candidate):
                    name = candidate.name.casefold()
                    return (name in {meta['exe'].casefold(), *(a.casefold() for a in meta['assets']),
                                     'diskmon.bin', 'offroad3.bin', 'newfont.raw', 'racestat.dec'}
                            or candidate.suffix.casefold() in ('.csh', '.omf') or name.isdecimal())

                files = {}
                for volume in roots:
                    for candidate in volume.rglob('*'):
                        if candidate.is_file() and not candidate.is_symlink() and game_file(candidate):
                            key = candidate.name.casefold()
                            if key not in files:
                                files[key] = candidate
                # Prefer files alongside the identified executable on collisions.
                files.update({p.name.casefold(): p for p in path.parent.iterdir()
                              if p.is_file() and not p.is_symlink() and game_file(p)})
                for asset in meta['assets']:
                    if asset.casefold() not in files or files[asset.casefold()].stat().st_size == 0:
                        raise ImportFailure(f'{meta["name"]}: missing {asset}; select the complete disk or game folder.')
                if title == 'hydro':
                    banks = [p for p in files.values() if p.suffix.casefold() == '.csh']
                    if len(banks) < 28 or any(p.stat().st_size < 65536 for p in banks):
                        raise ImportFailure('Hydro Thunder needs all 28 binary CSH sound banks. Re-import the original CHD.')
                return title, meta, files
    if unsupported:
        raise ImportFailure('Unsupported game revision. Use Hydro v01.01b or Offroad Thunder 2000-02-25.')
    raise ImportFailure('No supported game executable found. A BIOS-only ROM ZIP is not enough; select the game CHD or a complete extracted game folder/archive.')


def install(source, root):
    source, root = Path(source).resolve(), Path(root).resolve()
    if not source.exists():
        raise ImportFailure('The selected file or folder no longer exists.')
    root.mkdir(parents=True, exist_ok=True)
    # On the destination disk: avoids a multi-gigabyte image on a small /tmp tmpfs.
    with tempfile.TemporaryDirectory(prefix='.vct-import-', dir=root) as temporary:
        work = Path(temporary)
        roots = ([source] if source.is_dir() else extract_chd(source, work)
                 if source.suffix.casefold() == '.chd' else extract_archive(source, work))
        report(70, 'Checking game version and required files...')
        title, meta, files = identify(roots)
        destination = root / title
        if destination.exists():
            raise ImportFailure(f'{meta["name"]} already exists at {destination}. Existing game files and saves were kept.')
        stage = work / 'install'
        stage.mkdir()
        total = sum(p.stat().st_size for p in files.values()) or 1
        copied = 0
        for path in files.values():
            # Preserve binary data and original spelling; game file lookup is case-insensitive.
            shutil.copy2(path, stage / path.name)
            copied += path.stat().st_size
            report(70 + copied * 29 // total, f'Installing {meta["name"]}: {path.name}')
        if destination.exists():
            raise ImportFailure(f'{meta["name"]} was installed by another import. Existing files were kept.')
        stage.rename(destination)
        report(100, f'{meta["name"]} installed. Ready to play.')
        print(f'DONE {title}', flush=True)
        return title


def main():
    def cancelled(signum, frame):
        raise ImportFailure('Import cancelled. No installed game files were changed.')
    signal.signal(signal.SIGTERM, cancelled)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='data')
    parser.add_argument('source')
    args = parser.parse_args()
    try:
        install(args.source, args.root)
    except (ImportFailure, OSError, zipfile.BadZipFile) as error:
        print(f'ERROR {error}', flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
