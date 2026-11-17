#!/usr/bin/env python3
"""Read-only local diagnostic for DamnationX and owned Diablo installations.

Requires Python 3.8 or newer; uses only the standard library. No network access,
registry writes, game launches, save reads, file modification, or installation.
Run: py -3 damnationx_steam_diagnostic.py
Optional: py -3 damnationx_steam_diagnostic.py --d2r "D:\\SteamLibrary\\steamapps\\common\\Diablo II Resurrected" --diablo "C:\\Games\\Diablo" --launcher-dir "C:\\Games\\DamnationX"
"""
import argparse
import datetime
import json
import os
import platform
import re
import sys
from pathlib import Path

MAX_TEXT = 1024 * 1024
REPORT = []


def say(text=""):
    REPORT.append(str(text))


def safe_path(path):
    """Hide common home/account names; custom folder names may still identify you."""
    s = str(path)
    for home in (os.environ.get("USERPROFILE"), os.environ.get("HOME")):
        if home:
            s = re.sub(re.escape(home.rstrip("\\/")), r"<HOME>", s, flags=re.I)
    s = re.sub(r"(?i)([a-z]:[\\/]users[\\/])[^\\/]+", r"\1<USER>", s)
    return s


def child(directory, name):
    """Find a single case-insensitive child without traversing the tree."""
    try:
        return next((p for p in directory.iterdir() if p.name.lower() == name.lower()), directory / name)
    except OSError:
        return directory / name


def info(path, signature=False):
    try:
        if not path.exists():
            return "missing"
        if path.is_dir():
            return "directory, readable=" + str(os.access(path, os.R_OK))
        if not path.is_file():
            return "not a regular file"
        desc = "file, size={} bytes, readable={}".format(path.stat().st_size, os.access(path, os.R_OK))
        if signature:
            with path.open("rb") as f:
                sig = f.read(4)
            desc += ", signature=" + sig.hex(" ").upper()
            if sig[:2] == b"MZ":
                desc += " (Windows executable header)"
            elif sig in (b"MPQ\x1a", b"MPQ\x1b"):
                desc += " (MPQ archive header)"
            elif sig[:4] == b"\x7fELF":
                desc += " (ELF executable header)"
        return desc
    except OSError as e:
        return "access error: {} (errno {})".format(type(e).__name__, e.errno)


def read_small(path):
    if path.stat().st_size > MAX_TEXT:
        raise ValueError("file exceeds 1 MiB limit")
    return path.read_text(encoding="utf-8-sig", errors="replace")


def build_info(path):
    try:
        lines = read_small(path).splitlines()
        if len(lines) < 2:
            return "present but header/row missing"
        names = [v.split("!", 1)[0].strip().lower() for v in lines[0].split("|")]
        rows = [line.split("|") for line in lines[1:] if line.strip()]
        if not rows:
            return "present but no build rows"
        active_index = names.index("active") if "active" in names else -1
        active = [r for r in rows if active_index >= 0 and len(r) > active_index and r[active_index].strip() == "1"]
        row = active[0] if active else rows[0]
        # Never copy keys, CDN hosts, account IDs, or the full metadata file.
        allowed = ("version", "product", "branch", "active")
        values = []
        for key in allowed:
            if key in names and names.index(key) < len(row):
                value = row[names.index(key)].strip()
                if re.fullmatch(r"[A-Za-z0-9._ -]{1,100}", value):
                    values.append(key + "=" + value)
        return ", ".join(values) if values else "present; no safe version fields found"
    except (OSError, ValueError) as e:
        return "could not read: " + type(e).__name__


def inventory(directory):
    try:
        entries = list(directory.iterdir())
        files = [p for p in entries if p.is_file()]
        counts = {}
        size = 0
        unreadable = 0
        for p in files:
            ext = p.suffix.lower() or "<no extension>"
            # Limit output to the archive types needed for diagnosis.
            bucket = ext if ext in (".idx", ".index", ".data", ".config") or re.fullmatch(r"\.\d{3}", ext) else "other"
            counts[bucket] = counts.get(bucket, 0) + 1
            try:
                size += p.stat().st_size
            except OSError:
                unreadable += 1
        return "files={}, bytes={}, types={}, stat_errors={}".format(len(files), size, json.dumps(counts, sort_keys=True), unreadable)
    except OSError as e:
        return "cannot list directory: {} (errno {})".format(type(e).__name__, e.errno)


def steam_roots():
    roots = []
    if sys.platform == "win32":
        try:
            import winreg
            for hive, key, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                     (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
                try:
                    with winreg.OpenKey(hive, key) as handle:
                        value, _ = winreg.QueryValueEx(handle, name)
                        roots.append(Path(value))
                except OSError:
                    pass
        except ImportError:
            pass
        for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
            if base:
                roots.append(Path(base) / "Steam")
        # Common secondary libraries; users can always supply another path.
        for letter in "CDEFGHIJKLMNOPQRSTUVWXYZ":
            for suffix in ("SteamLibrary", "Steam"):
                p = Path(letter + ":\\") / suffix
                if p.is_dir():
                    roots.append(p)
    else:
        roots += [Path.home() / ".steam/steam", Path.home() / ".local/share/Steam",
                  Path.home() / "Library/Application Support/Steam"]
    libraries = list(roots)
    for root in roots:
        file = root / "steamapps" / "libraryfolders.vdf"
        try:
            text = read_small(file)
            # Valve VDF escapes Windows backslashes; do not log arbitrary fields.
            for match in re.finditer(r'"path"\s*"((?:\\.|[^"\\])*)"', text, re.I):
                value = match.group(1).replace("\\\\", "\\").replace('\\"', '"')
                libraries.append(Path(value))
        except (OSError, ValueError):
            continue
    unique = []
    seen = set()
    for path in libraries:
        key = os.path.normcase(str(path))
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def diagnose_d2r(root):
    say("\n[D2R installation] " + safe_path(root))
    if not root.is_dir():
        say("Folder: missing or inaccessible")
        return
    data = child(root, "Data")
    build = child(root, ".build.info")
    config = child(data, ".build.config")
    say("Root .build.info: " + info(build))
    if build.is_file():
        say("Safe build metadata: " + build_info(build))
    say("Data directory: " + info(data))
    if data.is_dir():
        say("Data root inventory: " + inventory(data))
    say("Data/.build.config: " + info(config))
    for dirname in ("indices", "data", "config"):
        path = child(data, dirname)
        say("Data/{}: {}".format(dirname, info(path)))
        if path.is_dir():
            say("  " + inventory(path))
    for name in ("D2R.exe", "Diablo II Resurrected Launcher.exe"):
        say(name + ": " + info(child(root, name), True))
    if build.is_file() and data.is_dir():
        say("Layout: matches the current master launcher's Battle.net-style .build.info + Data check.")
    elif config.is_file() and data.is_dir():
        say("Layout: Steam-like Data/.build.config without root .build.info; current master rejects this layout. This check alone does not establish import compatibility.")
    else:
        say("Layout: neither recognized marker combination was found; verify the installation root.")


def diagnose_diablo(path):
    say("\n[Diablo game data] " + safe_path(path))
    archive = path if path.is_file() else child(path, "DIABDAT.MPQ")
    say("DIABDAT.MPQ: " + info(archive, True))
    if path.is_dir():
        say("spawn.mpq: " + info(child(path, "spawn.mpq"), True))
        say("Original Diablo.exe: " + info(child(path, "Diablo.exe"), True))


def diagnose_launcher(root):
    say("\n[DamnationX release folder] " + safe_path(root))
    if not root.is_dir():
        say("Folder: missing or inaccessible")
        return
    for name in ("launcher.exe", "damnationx_host.exe", "devilutionx.exe", "PACKAGE.json"):
        say(name + ": " + info(child(root, name), name.lower().endswith(".exe")))
    for name in ("d2rtool.exe", "d2r-convert.exe"):
        say("resources/d2r-import/bin/{}: {}".format(name, info(root / "resources" / "d2r-import" / "bin" / name, True)))


def launcher_settings(path):
    say("\n[Launcher settings] " + safe_path(path))
    if not path.is_file():
        say("Not found; a custom --settings location or another launcher version may be in use.")
        return
    try:
        obj = json.loads(read_small(path))
        if not isinstance(obj, dict):
            raise ValueError("not an object")
        # An explicit allowlist avoids printing tokens, legacy options, or saves.
        for key in ("version", "runtime", "render_mode", "use_hd", "use_d2r", "use_models", "use_environment", "rt", "rt_quality"):
            if key in obj and isinstance(obj[key], (bool, int, float, str)):
                say("{}: {}".format(key, str(obj[key])[:80]))
        for key in ("game", "data_dir", "d2r_install", "d2r_dir", "importer"):
            value = obj.get(key)
            if isinstance(value, str) and value:
                p = Path(value)
                say("{}: {} ({})".format(key, safe_path(p), info(p, key == "game")))
                if key == "game":
                    basename = re.split(r"[\\/]", value)[-1].lower()
                    if basename.endswith(".mpq"):
                        say("  ISSUE: the game executable points to an MPQ data archive, which Windows cannot execute (error 193).")
                    elif basename == "diablo.exe":
                        say("  ISSUE: the selected executable appears to be original GOG Diablo, not the DamnationX host.")
        say("The original settings file and its other fields were not copied.")
    except (OSError, ValueError, json.JSONDecodeError) as e:
        say("Unable to read settings: " + type(e).__name__)


def ask_path(question):
    try:
        value = input(question).strip().strip('"')
    except (EOFError, KeyboardInterrupt):
        return None
    return Path(value).expanduser() if value else None


def main():
    parser = argparse.ArgumentParser(description="Read-only DamnationX diagnostic; no files are uploaded.")
    parser.add_argument("--d2r", help="D2R installation root")
    parser.add_argument("--diablo", help="Diablo 1 folder or DIABDAT.MPQ")
    parser.add_argument("--launcher-dir", help="Extracted DamnationX release folder")
    parser.add_argument("--settings", help="Launcher settings.json, if in a custom location")
    parser.add_argument("--output", default="damnationx-diagnostic.txt", help="Output text file (default: current folder)")
    parser.add_argument("--no-prompt", action="store_true", help="Never ask for paths")
    args = parser.parse_args()
    print("Read-only diagnostic. No uploads, game launches, game data, save files, or raw log contents are included.")
    print("The report includes redacted paths and system details. Review it before posting publicly.")
    say("DamnationX diagnostic, version 1")
    say("Generated: " + datetime.datetime.now(datetime.timezone.utc).isoformat())
    say("OS: {} {}; architecture: {}; Python: {}".format(platform.system(), platform.release(), platform.machine(), platform.python_version()))
    say("Paths are partially redacted; inspect this report for sensitive custom folder names before sharing.")
    libraries = steam_roots()
    say("\n[Steam libraries]")
    candidates = []
    for library in libraries:
        if library.is_dir():
            candidate = child(child(child(library, "steamapps"), "common"), "Diablo II Resurrected")
            say("{}: D2R {}".format(safe_path(library), "found" if candidate.is_dir() else "not found"))
            if candidate.is_dir():
                candidates.append(candidate)
    if not any(p.is_dir() for p in libraries):
        say("No Steam library found automatically.")
    d2r = Path(args.d2r).expanduser() if args.d2r else None
    if d2r is None and not candidates and not args.no_prompt:
        d2r = ask_path("Paste your D2R installation folder (or press Enter to skip): ")
    if d2r:
        candidates.insert(0, d2r)
    seen = set()
    for root in candidates:
        key = os.path.normcase(str(root))
        if key not in seen:
            seen.add(key)
            diagnose_d2r(root)
    if not candidates:
        say("\n[D2R installation] Not supplied or found.")
    diablo = Path(args.diablo).expanduser() if args.diablo else None
    if diablo is None and not args.no_prompt:
        diablo = ask_path("Paste your Diablo 1/GOG folder or DIABDAT.MPQ path (or Enter to skip): ")
    if diablo:
        diagnose_diablo(diablo)
    launcher = Path(args.launcher_dir).expanduser() if args.launcher_dir else None
    if launcher is None and not args.no_prompt:
        launcher = ask_path("Paste your extracted DamnationX folder (or Enter to skip): ")
    if launcher:
        diagnose_launcher(launcher)
    default_settings = (Path(os.environ["LOCALAPPDATA"]) / "damnationx/launcher/settings.json") if os.environ.get("LOCALAPPDATA") else (Path.home() / ".config/damnationx/launcher/settings.json")
    launcher_settings(Path(args.settings).expanduser() if args.settings else default_settings)
    say("\n[Logs]")
    say("Raw logs and save files are not collected. To provide an error, paste it separately after reviewing private paths or account information.")
    output = Path(args.output).expanduser()
    try:
        # Exclusive creation avoids overwriting a file the player already has.
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(REPORT) + "\n")
    except FileExistsError:
        output = output.with_name(output.stem + "-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + output.suffix)
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(REPORT) + "\n")
    print("Saved:", output.resolve())
    print("Open the report, review it, and attach it to Discord if you choose.")
    if sys.stdin.isatty() and not args.no_prompt:
        try:
            input("Press Enter to close.")
        except (EOFError, KeyboardInterrupt):
            pass


if __name__ == "__main__":
    main()
