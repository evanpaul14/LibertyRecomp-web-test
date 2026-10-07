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
import pathlib


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
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
    args = parser.parse_args()

    handler = functools.partial(IsolatedHandler, directory=args.directory)
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f"Serving {args.directory} at http://{args.bind}:{args.port}/")
        server.serve_forever()


if __name__ == "__main__":
    main()
