#!/usr/bin/env python3
"""CLI: python3 run.py [manifest.json] [out_dir]"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "py"))
from api import run  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "manifest_path",
        nargs="?",
        default=str(Path(__file__).parent / "designs" / "mempool_group.json"),
        help="design manifest JSON",
    )
    ap.add_argument(
        "output_dir",
        nargs="?",
        default=str(Path(__file__).parent / "out" / "latest"),
        help="output directory",
    )
    args = ap.parse_args()
    run(args.manifest_path, args.output_dir)
    print(args.output_dir)


if __name__ == "__main__":
    main()
