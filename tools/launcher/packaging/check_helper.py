#!/usr/bin/env python3
"""Check a native importer executable without opening any owned game data."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("helper", type=Path)
    args = parser.parse_args()
    helper = args.helper.resolve(strict=True)
    for flag in ("--version", "--help"):
        result = subprocess.run([str(helper), flag], capture_output=True, text=True,
                                check=True, timeout=15)
        output = result.stdout + result.stderr
        if "d2rtool" not in output.lower():
            parser.exit(1, f"{helper.name} {flag} did not identify the native helper\n")
        print(output.strip())


if __name__ == "__main__":
    main()
