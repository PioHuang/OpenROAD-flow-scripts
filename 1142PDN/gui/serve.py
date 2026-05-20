#!/usr/bin/env python3
"""
Serve the 1142PDN folder so the GUI can fetch ``/out/latest/ir_mesh_nodes.tsv``.

From ``OpenROAD-flow-scripts/1142PDN``::

  python3 gui/serve.py

If the default port is busy, the next free port is used (see printed URL).
"""

from __future__ import annotations

import argparse
import errno
import functools
import http.server
import socketserver
from pathlib import Path


class ReuseAddrTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(description="1142PDN static GUI server")
    parser.add_argument("--host", default="127.0.0.1", help="bind address")
    parser.add_argument(
        "--port",
        type=int,
        default=8091,
        help="TCP port (if busy, tries the next ports up to +64)",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=str(root)
    )

    def addr_in_use(exc: OSError) -> bool:
        if exc.errno == errno.EADDRINUSE:
            return True
        if getattr(exc, "winerror", None) == 10048:
            return True
        return False

    start = args.port if args.port > 0 else 8091
    httpd = None
    bound_port = None
    for candidate in range(start, start + 64):
        try:
            httpd = ReuseAddrTCPServer((args.host, candidate), handler)
            bound_port = candidate
            break
        except OSError as e:
            if not addr_in_use(e):
                raise
            continue
    if httpd is None or bound_port is None:
        raise SystemExit(
            f"No free TCP port in range {start}..{start + 63} on {args.host}"
        )

    if bound_port != args.port and args.port > 0:
        print(
            f"Note: port {args.port} was in use; bound to {bound_port} instead.",
            flush=True,
        )

    try:
        print(f"Serving {root}", flush=True)
        print(
            f"  GUI: http://{args.host}:{bound_port}/gui/index.html",
            flush=True,
        )
        print(
            f"  (or http://{args.host}:{bound_port}/ for redirect)",
            flush=True,
        )
        print("  Ctrl+C to stop", flush=True)
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.", flush=True)
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
