"""Installer safety checks. All external commands, including sudo, are mocked."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'linfxp_package', Path(__file__).resolve().parents[1] / 'scripts/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageInstaller(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='linfxp-package-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'VERSION').write_text('0.3.0\n')
        recipe = self.root / 'packaging/arch/PKGBUILD.in'
        recipe.parent.mkdir(parents=True)
        recipe.write_text('pkgver=@VERSION@\npkgrel=@PKGREL@\n')
        self.output = self.root / 'dist'
        self.commands = []
        self.fail_build = False
        self.produce_package = True
        for replacement in (
            patch.object(package, 'ROOT', self.root),
            patch.object(package, 'snapshot_files', return_value=[self.root / 'VERSION']),
            patch.object(package.os, 'geteuid', return_value=1000),
            patch.object(package.shutil, 'which', side_effect=lambda name: '/usr/bin/' + name),
            patch.object(package.subprocess, 'run', side_effect=self.run_command),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            replacement.__enter__()
            self.addCleanup(replacement.__exit__, None, None, None)

    def run_command(self, command, **kwargs):
        self.commands.append(command)
        if command[:2] == ['makepkg', '--cleanbuild']:
            if self.fail_build:
                raise subprocess.CalledProcessError(4, command)
            if self.produce_package:
                # A signature must never be passed to pacman as a package.
                for suffix in ('', '.sig'):
                    (kwargs['cwd'] / ('linfxp-0.3.0-1-x86_64.pkg.tar.zst' + suffix)).touch()
        return subprocess.CompletedProcess(command, 0, stdout='pkgbase = linfxp\n')

    def test_install_builds_first_and_installs_only_the_new_package(self):
        directory = package.build_snapshot(self.output, install=True)
        self.assertEqual(self.commands[1][0:2], ['makepkg', '--cleanbuild'])
        self.assertEqual(self.commands[2], [
            'sudo', 'pacman', '-U', '--',
            str(directory / 'linfxp-0.3.0-1-x86_64.pkg.tar.zst')])
        self.assertNotIn('--nocheck', self.commands[1])
        self.assertNotIn('--noconfirm', self.commands[2])

    def test_build_only_does_not_install(self):
        package.build_snapshot(self.output, build=True)
        self.assertFalse(any(command[0] == 'sudo' for command in self.commands))

    def test_failed_build_or_tests_never_installs(self):
        self.fail_build = True
        with self.assertRaises(subprocess.CalledProcessError):
            package.build_snapshot(self.output, install=True)
        self.assertFalse(any(command[0] == 'sudo' for command in self.commands))

    def test_missing_package_never_installs(self):
        self.produce_package = False
        with self.assertRaisesRegex(RuntimeError, 'No package found'):
            package.build_snapshot(self.output, install=True)
        self.assertFalse(any(command[0] == 'sudo' for command in self.commands))

    def test_running_as_root_is_rejected_before_building(self):
        with patch.object(package.os, 'geteuid', return_value=0):
            with self.assertRaisesRegex(ValueError, 'normal user'):
                package.build_snapshot(self.output, install=True)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.commands, [])

    def test_missing_tools_are_rejected_before_building(self):
        with patch.object(package.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'makepkg is required'):
                package.build_snapshot(self.output, install=True)
        self.assertEqual(self.commands, [])

    def test_install_cli_forwards_options(self):
        with patch.object(package, 'build_snapshot') as build:
            with patch('sys.argv', ['package.py', '--install', '--release', '7',
                                    '--output', str(self.output)]):
                self.assertEqual(package.main(), 0)
            build.assert_called_once_with(self.output, 7, False, True)


if __name__ == '__main__':
    unittest.main()
