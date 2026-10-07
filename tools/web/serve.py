#!/usr/bin/env python3
"""Serve the Liberty Recompiled web build locally.

WebAssembly threads need SharedArrayBuffer, which browsers only enable on
cross-origin isolated pages, so every response carries the COOP/COEP headers.
Python's built-in server does not send them, hence this wrapper.

Usage:
    python3 tools/web/serve.py [--port 8080] [directory]

The directory defaults to out/web/LibertyRecomp. Then open
http://localhost:8080/ (localhost counts as a secure context).
"""

import argparse
import functools
import http.server
import json
import os
import pathlib
import re
import urllib.parse


def default_game_dir():
    data = os.environ.get("XDG_DATA_HOME") or str(pathlib.Path.home() / ".local" / "share")
    return pathlib.Path(data) / "LibertyRecomp" / "game"


def build_manifest(root):
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            files.append([path.relative_to(root).as_posix(), path.stat().st_size])
    return json.dumps(files).encode()


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    game_dir = None
    manifest = None

    def do_GET(self):
        if self.game_dir and self.path.split("?")[0] == "/game-manifest.json":
            if IsolatedHandler.manifest is None:
                IsolatedHandler.manifest = build_manifest(self.game_dir)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(self.manifest)))
            self.end_headers()
            self.wfile.write(self.manifest)
        elif self.game_dir and self.path.startswith("/game/"):
            self.serve_game_file(urllib.parse.unquote(self.path[len("/game/"):].split("?")[0]))
        else:
            super().do_GET()

    def serve_game_file(self, rel):
        target = (self.game_dir / rel).resolve()
        if self.game_dir.resolve() not in target.parents or not target.is_file():
            self.send_error(404)
            return
        size = target.stat().st_size
        start, end = 0, size - 1
        match = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
        if match and (match[1] or match[2]):
            if match[1]:
                start = int(match[1])
                if match[2]:
                    end = min(int(match[2]), size - 1)
            else:
                start = max(0, size - int(match[2]))
        if start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return
        self.send_response(206 if match else 200)
        if match:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        with open(target, "rb") as handle:
            handle.seek(start)
            remaining = end - start + 1
            while remaining:
                chunk = handle.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", nargs="?",
                        default=str(repo_root / "out" / "web" / "LibertyRecomp"))
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--game", default=str(default_game_dir()),
                        help="installed game directory, exposed at /game/ for the page "
                             "(default: $XDG_DATA_HOME/LibertyRecomp/game)")
    args = parser.parse_args()

    game = pathlib.Path(args.game)
    IsolatedHandler.game_dir = game if game.is_dir() else None
    handler = functools.partial(IsolatedHandler, directory=args.directory)
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f"Serving {args.directory} at http://{args.bind}:{args.port}/")
        server.serve_forever()


if __name__ == "__main__":
    main()
