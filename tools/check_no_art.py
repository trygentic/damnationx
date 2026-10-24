#!/usr/bin/env python3
"""Fails when model or texture files under assets/ are committed (or staged).

The 3D models and their textures are Blizzard-derived or third-party art (the HD pack and the FREE 3D pack, see
docs/3d-asset-packs.md). They are distributed separately and must never be committed or pushed. Model files
(.glb, .gltf, .fbx, .blend, .obj, .dae) under assets/ are always rejected; texture files (.png, .jpg, .tga, .dds,
.ktx, .ktx2, .webp, .exr, .hdr, .psd) under assets/ are rejected unless tools/art_guard_allowlist.txt lists them
(original art that predates the guard, each entry reviewed).

Usage:
  tools/check_no_art.py            check every tracked file (CI)
  tools/check_no_art.py --staged   check the files staged for the next commit (pre-commit hook)

Install the hook once per clone: git config core.hooksPath tools/hooks
"""

import pathlib
import subprocess
import sys

MODEL_EXTENSIONS = {'.glb', '.gltf', '.fbx', '.blend', '.blend1', '.obj', '.dae'}
TEXTURE_EXTENSIONS = {'.png', '.jpg', '.jpeg', '.tga', '.dds', '.ktx', '.ktx2', '.webp', '.exr', '.hdr', '.psd', '.basis'}
ALLOWLIST = pathlib.Path(__file__).resolve().parent / 'art_guard_allowlist.txt'


def git_files(staged: bool) -> list[str]:
    if staged:
        # Added, copied, modified or renamed files only: deleting a model is always fine.
        command = ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z']
    else:
        command = ['git', 'ls-files', '-z']
    output = subprocess.run(command, check=True, capture_output=True).stdout.decode('utf-8')
    return [name for name in output.split('\0') if name]


def load_allowlist() -> set[str]:
    if not ALLOWLIST.exists():
        return set()
    entries = set()
    for line in ALLOWLIST.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            entries.add(line)
    return entries


def main() -> int:
    staged = '--staged' in sys.argv[1:]
    allowed = load_allowlist()
    violations = []
    for name in git_files(staged):
        if not name.startswith('assets/'):
            continue
        suffix = pathlib.PurePosixPath(name).suffix.lower()
        if suffix in MODEL_EXTENSIONS:
            violations.append((name, '3D model'))
        elif suffix in TEXTURE_EXTENSIONS and name not in allowed:
            violations.append((name, 'texture'))
    if not violations:
        return 0
    print('Model and texture files must not be committed under assets/ (they ship in the HD and FREE 3D packs):', file=sys.stderr)
    for name, kind in violations:
        print(f'  {name} ({kind})', file=sys.stderr)
    if staged:
        print('Unstage them with: git restore --staged <file>', file=sys.stderr)
    else:
        print('Remove them from the index with: git rm --cached <file>', file=sys.stderr)
    print('Original art that belongs in the repository goes in tools/art_guard_allowlist.txt after review.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
