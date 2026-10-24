#!/usr/bin/env python3
"""Archive a built launcher and optional source-only D2R importer recipes.

This tool neither builds the game nor reads an installed game's assets. Its
allowlist deliberately excludes converted models, textures, archives and caches.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
import tempfile
import tomllib
import zipfile


REPO = Path(__file__).resolve().parents[3]
IMPORTER_ROOT_FILES = (
    "README.md", "FORMATS.md", "UI.md", "mapping.toml", "env_mapping.toml",
    "requirements.txt",
)
IMPORTER_SOURCE_DIRS = {
    "d2r_import": {".py"},
    "native": {".py", ".sh", ".cpp", ".c", ".h", ".hpp", ".cmake", ".md", ".txt"},
    "scripts": {".py", ".cjs"},
}
IMPORTER_REQUIRED_FILES = (
    "d2r_import/__main__.py", "mapping.toml", "env_mapping.toml", "requirements.txt",
    "native/build.py", "native/CMakeLists.txt",
)
IGNORED_SOURCE_DIRS = {"__pycache__", ".git", ".venv", "target", "build", "dist", "_deps", "cache", "raw", "glb", "bin"}
EXECUTABLE_FILES = {
    "launcher", "launcher.exe",
    "resources/d2r-import/bin/d2rtool", "resources/d2r-import/bin/d2rtool.exe",
}


def token(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value):
        raise ValueError(f"invalid archive name component: {value!r}")
    return value


def copy_file(source: Path, target: Path, *, executable: bool = False) -> None:
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"expected a regular, non-symlink file: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o755 if executable else 0o644)


def copy_importer(source: Path, target: Path) -> None:
    if source.is_symlink() or not source.is_dir():
        raise ValueError(f"invalid importer source folder: {source}")
    for relative in IMPORTER_REQUIRED_FILES:
        if not (source / relative).is_file():
            raise ValueError(f"incomplete importer source recipe: {source / relative}")
    for name in IMPORTER_ROOT_FILES:
        if (source / name).exists():
            copy_file(source / name, target / name)
    for name, suffixes in IMPORTER_SOURCE_DIRS.items():
        directory = source / name
        if directory.is_symlink():
            raise ValueError(f"refusing importer source symlink: {directory}")
        for item in sorted(directory.rglob("*")):
            directories = item.relative_to(directory).parts[:-1]
            if any(part in IGNORED_SOURCE_DIRS or part.startswith("build-") for part in directories):
                continue
            if item.is_symlink():
                raise ValueError(f"refusing importer source symlink: {item}")
            if item.is_file() and item.suffix.lower() in suffixes:
                # Python bytecode, build products and every bitmap/model format
                # are absent from the extension allowlist.
                copy_file(item, target / item.relative_to(source))


def copy_helper(source: Path, target: Path) -> None:
    if source.name not in {"d2rtool", "d2rtool.exe"}:
        raise ValueError("--helper must select d2rtool or d2rtool.exe")
    copy_file(source, target / source.name, executable=True)
    licenses = source.parent / "licenses"
    required = ("CascLib-LICENSE.txt", "CascLib-zlib.h", "BitKnit2-LICENSE.txt")
    if licenses.is_symlink():
        raise ValueError(f"refusing helper license symlink: {licenses}")
    for name in required:
        copy_file(licenses / name, target / "licenses" / name)
    metadata = source.with_name("d2rtool.build.json")
    if metadata.exists():
        copy_file(metadata, target / metadata.name)


def archive_tree(staged: Path, destination: Path, windows: bool) -> None:
    files = sorted(path for path in staged.rglob("*") if path.is_file())
    if windows:
        with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in files:
                entry = zipfile.ZipInfo((Path(staged.name) / path.relative_to(staged)).as_posix())
                entry.create_system = 3
                # Windows chmod/stat cannot express Unix execute bits. Encode
                # package roles explicitly, independent of the build host.
                mode = 0o755 if path.relative_to(staged).as_posix() in EXECUTABLE_FILES else 0o644
                entry.external_attr = (stat.S_IFREG | mode) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, path.read_bytes())
    else:
        # Normalize timestamps, owners and gzip headers for reproducible bytes.
        with destination.open("wb") as output, gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in files:
                    entry = archive.gettarinfo(str(path), arcname=(Path(staged.name) / path.relative_to(staged)).as_posix())
                    entry.mode = 0o755 if path.relative_to(staged).as_posix() in EXECUTABLE_FILES else 0o644
                    entry.uid = entry.gid = entry.mtime = 0
                    entry.uname = entry.gname = ""
                    with path.open("rb") as data:
                        archive.addfile(entry, data)


def package(binary: Path, output_dir: Path, platform: str, *, version: str = "0.1.0",
            importer: Path | None = None, helper: Path | None = None,
            revision: str = "", repo: Path = REPO) -> Path:
    platform, version = token(platform), token(version)
    if helper is not None and importer is None:
        raise ValueError("a helper requires the importer source recipe")
    windows = platform.startswith("windows-")
    if helper is not None and (helper.suffix == ".exe") != windows:
        raise ValueError("helper executable suffix does not match the package platform")
    output_dir.mkdir(parents=True, exist_ok=True)
    name = f"damnationx-launcher-{version}-{platform}"
    destination = output_dir / f"{name}{'.zip' if windows else '.tar.gz'}"
    with tempfile.TemporaryDirectory(prefix=".launcher-package-", dir=output_dir) as temp:
        staged = Path(temp) / name
        staged.mkdir()
        copy_file(binary, staged / ("launcher.exe" if windows else "launcher"), executable=True)
        copy_file(repo / "tools/launcher/README.md", staged / "README.md")
        copy_file(repo / "LICENSE.md", staged / "LICENSE.md")
        if importer is not None:
            recipe = staged / "resources/d2r-import"
            copy_importer(importer, recipe)
            if helper is not None:
                copy_helper(helper, recipe / "bin")
        hashes = {
            path.relative_to(staged).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(staged.rglob("*")) if path.is_file()
        }
        manifest = {
            "schema": 1, "name": "DamnationX launcher", "version": version,
            "platform": platform, "revision": revision,
            "importer_recipe": importer is not None,
            "native_importer_helper": helper is not None,
            "files_sha256": hashes,
        }
        (staged / "PACKAGE.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
        temporary_archive = Path(temp) / destination.name
        archive_tree(staged, temporary_archive, windows)
        os.replace(temporary_archive, destination)
    checksum = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_name(destination.name + ".sha256").write_text(f"{checksum}  {destination.name}\n", encoding="utf-8", newline="\n")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="built launcher executable")
    parser.add_argument("--output-dir", type=Path, default=Path("dist/launcher"))
    parser.add_argument("--platform", required=True, help="e.g. windows-x64, macos-arm64, linux-x64")
    parser.add_argument("--version", default=tomllib.loads((REPO / "tools/launcher/Cargo.toml").read_text())["package"]["version"])
    parser.add_argument("--revision", default="", help="source commit recorded in PACKAGE.json")
    parser.add_argument("--importer", choices=("auto", "include", "omit"), default="auto",
                        help="auto includes source recipes when present in this checkout")
    parser.add_argument("--importer-root", type=Path, default=REPO / "tools/d2r-import")
    parser.add_argument("--helper", type=Path, help="optional built d2rtool(.exe), with sibling licenses/")
    args = parser.parse_args()
    importer = None
    if args.importer != "omit" and (args.importer == "include" or args.importer_root.exists()):
        importer = args.importer_root
    try:
        result = package(args.binary, args.output_dir, args.platform, version=args.version,
                         importer=importer, helper=args.helper, revision=args.revision)
    except (OSError, ValueError) as error:
        parser.exit(1, f"package: {error}\n")
    print(result)


if __name__ == "__main__":
    main()
