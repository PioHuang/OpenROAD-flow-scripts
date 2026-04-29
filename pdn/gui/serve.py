#!/usr/bin/env python3
import argparse
import http.server
import os
import socketserver
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve PDN GUI and dumped TSVs")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host")
    parser.add_argument("--port", type=int, default=8000, help="Bind port")
    parser.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Document root (default: pdn directory)",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    os.chdir(root)

    handler = http.server.SimpleHTTPRequestHandler
    with socketserver.TCPServer((args.host, args.port), handler) as httpd:
        print(f"Serving {root} at http://{args.host}:{args.port}/gui/index.html")
        httpd.serve_forever()


if __name__ == "__main__":
    main()

