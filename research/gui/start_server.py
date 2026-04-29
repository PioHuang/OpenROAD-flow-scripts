#!/usr/bin/env python3
"""
Research IR plot viewer: static HTML + http.server, plus /api/render to run research/scripts/plot.py on demand.

From the OpenROAD-flow-scripts repo root:

  python3 research/gui/start_server.py

Then open http://localhost:8765/  (default port; use --port to change).
"""

from __future__ import annotations

import argparse
import http.server
import os
import socketserver
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path


def repo_root_default() -> Path:
    return Path(__file__).resolve().parent.parent.parent


def gui_dir() -> Path:
    return Path(__file__).resolve().parent


def render_model3d_html(repo: Path, manifest_rel: str, mesh_rel: str) -> None:
    """Run plot.py model3d; writes research/out/ir_pdn_model_3d_{std,macro}.html"""
    script = repo / "research" / "scripts" / "plot.py"
    if not script.is_file():
        raise FileNotFoundError(f"Missing {script}")
    cmd = [
        sys.executable,
        str(script),
        "model3d",
        "--manifest",
        manifest_rel,
        "--mesh",
        str(repo / mesh_rel),
    ]
    subprocess.run(cmd, cwd=str(repo), check=True, capture_output=True, text=True)


def render_plot_png(repo: Path, kind: str, manifest_rel: str, mesh_rel: str) -> bytes:
    """Run plot.py in a subprocess; return PNG bytes."""
    script = repo / "research" / "scripts" / "plot.py"
    if not script.is_file():
        raise FileNotFoundError(f"Missing {script}")
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        out_path = tmp.name
    try:
        cmd = [
            sys.executable,
            str(script),
            kind,
            "--manifest",
            manifest_rel,
            "--mesh",
            str(repo / mesh_rel),
            "--out",
            out_path,
        ]
        subprocess.run(cmd, cwd=str(repo), check=True, capture_output=True, text=True)
        return Path(out_path).read_bytes()
    finally:
        try:
            os.unlink(out_path)
        except OSError:
            pass


def make_handler(repo: Path, manifest_rel: str, mesh_rel: str):
    gdir = gui_dir()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(gdir), **kwargs)

        def log_message(self, fmt, *args_):
            sys.stderr.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), fmt % args_))

        def end_headers(self) -> None:  # noqa: N802
            # Avoid stale JS/HTML in browser cache while iterating on the GUI.
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_GET(self) -> None:  # noqa: N802
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path

            if path in ("", "/"):
                self.send_response(302)
                self.send_header("Location", "/index.html")
                self.end_headers()
                return

            if path == "/api/render":
                qs = urllib.parse.parse_qs(parsed.query or "")
                kind = (qs.get("kind") or ["current"])[0]
                if kind not in ("current", "voltage", "sources", "model", "model3d"):
                    self.send_error(400, f"invalid kind: {kind}")
                    return
                man = (qs.get("manifest") or [manifest_rel])[0]
                mesh = (qs.get("mesh") or [mesh_rel])[0]
                if kind == "model3d":
                    try:
                        render_model3d_html(repo, man, mesh)
                    except subprocess.CalledProcessError as e:
                        msg = (e.stderr or e.stdout or str(e))[:8000]
                        self.send_response(500)
                        self.send_header("Content-Type", "text/plain; charset=utf-8")
                        self.end_headers()
                        self.wfile.write(b"plot.py model3d failed:\n\n")
                        self.wfile.write(msg.encode("utf-8", errors="replace"))
                        return
                    except Exception as e:
                        self.send_error(500, str(e))
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(
                        b'{"ok":true,"std":"ir_pdn_model_3d_std.html","macro":"ir_pdn_model_3d_macro.html"}'
                    )
                    return
                try:
                    body = render_plot_png(repo, kind, man, mesh)
                except subprocess.CalledProcessError as e:
                    msg = (e.stderr or e.stdout or str(e))[:8000]
                    self.send_response(500)
                    self.send_header("Content-Type", "text/plain; charset=utf-8")
                    self.end_headers()
                    self.wfile.write(b"plot.py failed:\n\n")
                    self.wfile.write(msg.encode("utf-8", errors="replace"))
                    return
                except Exception as e:
                    self.send_error(500, str(e))
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return

            if path.startswith("/out/"):
                name = os.path.basename(path)
                if name != path.rsplit("/", 1)[-1] or ".." in path:
                    self.send_error(400, "bad path")
                    return
                fp = repo / "research" / "out" / name
                if not fp.is_file():
                    self.send_error(404, f"not found: {name}")
                    return
                data = fp.read_bytes()
                self.send_response(200)
                if name.endswith(".png"):
                    self.send_header("Content-Type", "image/png")
                elif name.endswith(".html"):
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                elif name.endswith(".tsv"):
                    self.send_header("Content-Type", "text/tab-separated-values; charset=utf-8")
                else:
                    self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)
                return

            super().do_GET()

    return Handler


def main() -> None:
    ap = argparse.ArgumentParser(description="Research IR plot web GUI (HTTP server + on-demand plots).")
    ap.add_argument("--host", default="0.0.0.0", help="Bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8765, help="Port (default 8765)")
    ap.add_argument("--repo", type=Path, default=None, help="OpenROAD-flow-scripts repo root (default: parent of research/)")
    ap.add_argument(
        "--manifest",
        default="research/mempool.json",
        help="Manifest path relative to repo (default research/mempool.json)",
    )
    ap.add_argument(
        "--mesh",
        default="research/out/ir_mesh_nodes.tsv",
        help="Mesh TSV path relative to repo (default research/out/ir_mesh_nodes.tsv)",
    )
    args = ap.parse_args()
    repo = args.repo.resolve() if args.repo else repo_root_default()
    if not (repo / "research").is_dir():
        sys.exit(f"--repo does not look like ORFS root (no research/): {repo}")

    handler = make_handler(repo, args.manifest, args.mesh)
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    httpd = socketserver.ThreadingTCPServer((args.host, args.port), handler)
    url_host = "localhost" if args.host in ("0.0.0.0", "::") else args.host
    print(f"Serving research/gui on http://{url_host}:{args.port}/")
    print(f"  On-demand plots: /api/render?kind=current|voltage|sources|model|model3d")
    print(f"  PDN Tcl canvas: /out/pdn_tcl_physical.tsv (from phys_load) + GUI button \"PDN Tcl physical\"")
    print(f"  Static files from: {gui_dir()}")
    print(f"  Repo root: {repo}")
    print("Ctrl+C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
