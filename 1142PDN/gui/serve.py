#!/usr/bin/env python3
import argparse
import http.server
import os
import socketserver
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Serve 1142PDN GUI")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", default=8010, type=int)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    args = parser.parse_args()

    root = Path(args.root).resolve()
    os.chdir(root)
    with socketserver.TCPServer((args.host, args.port), http.server.SimpleHTTPRequestHandler) as httpd:
        print(f"Serving {root} at http://{args.host}:{args.port}/1142PDN/gui/index.html")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down GUI server.")
            httpd.shutdown()


if __name__ == "__main__":
    main()
