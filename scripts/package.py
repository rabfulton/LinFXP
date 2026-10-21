#!/usr/bin/env python3
"""Build an immutable Arch/Manjaro package; optionally install it with sudo.

--build builds and tests only. --install also installs the resulting package.
Neither mode installs dependencies or downloads source changes.
Each run creates a new package revision unless --release is explicitly supplied.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = (
    'VERSION', 'CHANGELOG.md', 'README.md', 'LICENSE', 'CMakeLists.txt',
    'resources.qrc', '.clang-format', '.gitignore', 'install.sh',
)
SOURCE_DIRECTORIES = ('src', 'worker', 'tests', 'packaging', 'scripts', 'screenshots')


def snapshot_files():
    paths = [ROOT / name for name in SOURCE_FILES]
    for name in SOURCE_DIRECTORIES:
        paths.extend(p for p in (ROOT / name).rglob('*')
                     if p.is_file() and not p.is_symlink()
                     and '__pycache__' not in p.parts and p.suffix != '.pyc')
    for path in paths:
        if not path.is_file() or path.is_symlink():
            raise ValueError('Missing or unsafe source file: ' + str(path))
    return sorted(paths)


def build_snapshot(output, release=None, build=False, install=False):
    if install:
        if os.geteuid() == 0:
            raise ValueError('Run install.sh as your normal user, not with sudo; '
                             'only package installation needs root.')
        for command in ('makepkg', 'pacman', 'sudo'):
            if not shutil.which(command):
                raise RuntimeError(f'{command} is required for installation; '
                                   'use this script on an Arch-based system with build dependencies installed.')
        build = True
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('VERSION must contain a numeric major.minor.patch version')
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if release is None:
        existing = [int(match[1]) for p in output.iterdir()
                    if p.is_dir() and (match := re.fullmatch(
                        re.escape('linfxp-' + version + '-') + r'(\d+)', p.name))]
        release = max(existing, default=0) + 1
    if release < 1:
        raise ValueError('Package release must be positive')
    directory = output / f'linfxp-{version}-{release}'
    directory.mkdir(exist_ok=False)
    archive = directory / f'linfxp-{version}.tar.gz'
    files = snapshot_files()
    with tarfile.open(archive, 'w:gz') as tar:
        for path in files:
            tar.add(path, arcname=f'linfxp-{version}/' + str(path.relative_to(ROOT)), recursive=False)
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    recipe = (ROOT / 'packaging/arch/PKGBUILD.in').read_text()
    replacements = {'VERSION': version, 'PKGREL': str(release), 'SHA256': checksum,
                    'PROJECT_URL': ROOT.as_uri()}
    for key, value in replacements.items():
        recipe = recipe.replace('@' + key + '@', value)
    (directory / 'PKGBUILD').write_text(recipe)
    print(f'Prepared {version}-{release}: {directory}', flush=True)
    if not shutil.which('makepkg'):
        if build:
            raise RuntimeError('makepkg is unavailable; build the prepared recipe on Arch/Manjaro')
        return directory
    source_info = subprocess.run(['makepkg', '--printsrcinfo'], cwd=directory, check=True,
                                 text=True, capture_output=True).stdout
    (directory / '.SRCINFO').write_text(source_info)
    if build:
        # Do not use --syncdeps or --install: this command only builds a package.
        subprocess.run(['makepkg', '--cleanbuild', '--clean', '--noconfirm', '--noprogressbar'],
                       cwd=directory, check=True)
        packages = sorted(p for p in directory.glob('linfxp-*.pkg.tar.*')
                          if p.is_file() and p.suffix != '.sig')
        if not packages:
            raise RuntimeError('No package found; check whether makepkg.conf redirects PKGDEST')
        for package in packages:
            print(f'Package: {package}', flush=True)
            if not install:
                print(f'Install/upgrade: sudo pacman -U "{package}"', flush=True)
        if install:
            print('Build and tests passed. Installing with sudo; review the pacman prompt.', flush=True)
            subprocess.run(['sudo', 'pacman', '-U', '--', *(str(p) for p in packages)], check=True)
            print('LinFXP installed. Restart the application to use the updated version.', flush=True)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true', help='run makepkg and its tests; never install')
    parser.add_argument('--install', action='store_true', help='build and test, then install with sudo pacman -U')
    parser.add_argument('--release', type=int, help='explicit pkgrel (default: next unused local revision)')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist', help='output directory (default: dist/)')
    arguments = parser.parse_args()
    try:
        build_snapshot(arguments.output, arguments.release, arguments.build, arguments.install)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Cancelled.', file=sys.stderr)
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
