"""Artifact-content checks use synthetic executables and source recipes only."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from package import IMPORTER_REQUIRED_FILES, package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tools/launcher").mkdir(parents=True)
        (self.root / "tools/launcher/README.md").write_text("Launcher instructions\n")
        (self.root / "LICENSE.md").write_text("Project terms\n")
        self.binary = self.root / "launcher-binary"
        self.binary.write_bytes(b"synthetic executable\n")
        self.output = self.root / "dist"

    def recipe(self):
        source = self.root / "importer"
        for name in IMPORTER_REQUIRED_FILES:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("# synthetic source recipe\n")
        return source

    @patch("package.Path.chmod", return_value=None)
    def test_windows_archive_has_only_requested_files_and_verified_inventory(self, _chmod):
        recipe = self.recipe()
        for name in ("raw/secret.bin", "glb/owned.glb", "native/owned.png", "d2r_import/owned.texture"):
            target = recipe / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"must not ship")
        archive = package(self.binary, self.output, "windows-x64", importer=recipe, repo=self.root)
        with zipfile.ZipFile(archive) as output:
            names = output.namelist()
            prefix = names[0].split("/")[0] + "/"
            self.assertIn(prefix + "launcher.exe", names)
            self.assertIn(prefix + "resources/d2r-import/native/CMakeLists.txt", names)
            self.assertFalse(any("owned" in name or "/raw/" in name for name in names))
            for entry in output.infolist():
                expected_mode = 0o755 if entry.filename == prefix + "launcher.exe" else 0o644
                self.assertEqual((entry.external_attr >> 16) & 0o777, expected_mode)
                self.assertEqual((entry.external_attr >> 16) & 0o170000, 0o100000)
            manifest = json.loads(output.read(prefix + "PACKAGE.json"))
            for name, digest in manifest["files_sha256"].items():
                self.assertEqual(hashlib.sha256(output.read(prefix + name)).hexdigest(), digest)
            self.assertEqual(len(names), len(manifest["files_sha256"]) + 1)
        self.assertTrue(archive.with_name(archive.name + ".sha256").is_file())

    @patch("package.Path.chmod", return_value=None)
    def test_unix_archive_keeps_executable_permission_and_is_reproducible(self, _chmod):
        archive = package(self.binary, self.output, "linux-x64", repo=self.root)
        before = archive.read_bytes()
        with tarfile.open(archive) as output:
            member = next(item for item in output.getmembers() if item.name.endswith("/launcher"))
            self.assertEqual(member.mode & 0o777, 0o755)
            self.assertTrue(all(item.mode == 0o644 for item in output.getmembers() if not item.name.endswith("/launcher")))
            self.assertTrue(all(item.isfile() for item in output.getmembers()))
        self.assertEqual(package(self.binary, self.output, "linux-x64", repo=self.root).read_bytes(), before)

    @patch("package.Path.chmod", return_value=None)
    def test_helper_requires_recipe_and_exact_third_party_notices(self, _chmod):
        helper = self.root / "bin/d2rtool"
        helper.parent.mkdir()
        helper.write_bytes(b"synthetic helper")
        with self.assertRaises(ValueError):
            package(self.binary, self.output, "macos-arm64", helper=helper, repo=self.root)
        recipe = self.recipe()
        with self.assertRaises(ValueError):
            package(self.binary, self.output, "macos-arm64", importer=recipe, helper=helper, repo=self.root)
        for name in ("CascLib-LICENSE.txt", "CascLib-zlib.h", "BitKnit2-LICENSE.txt"):
            target = helper.parent / "licenses" / name
            target.parent.mkdir(exist_ok=True)
            target.write_text("synthetic third-party notice\n")
        archive = package(self.binary, self.output, "macos-arm64", importer=recipe, helper=helper, repo=self.root)
        with tarfile.open(archive) as output:
            self.assertEqual(sum(item.name.endswith("/d2rtool") for item in output.getmembers()), 1)
            self.assertEqual(sum("/licenses/" in item.name for item in output.getmembers()), 3)
            for item in output.getmembers():
                expected_mode = 0o755 if item.name.endswith(("/launcher", "/d2rtool")) else 0o644
                self.assertEqual(item.mode, expected_mode)

    def test_rejects_symlinks_in_recipe_and_archive_name_traversal(self):
        with self.assertRaises(ValueError):
            package(self.binary, self.output, "../escape", repo=self.root)
        recipe = self.recipe()
        link = recipe / "d2r_import/linked.py"
        try:
            link.symlink_to(self.binary)
        except OSError:
            self.skipTest("symlink creation is unavailable for this account")
        with self.assertRaises(ValueError):
            package(self.binary, self.output, "linux-x64", importer=recipe, repo=self.root)


if __name__ == "__main__":
    unittest.main()
