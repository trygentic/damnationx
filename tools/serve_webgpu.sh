#!/usr/bin/env bash
# Serve an Emscripten WebGPU build directory for manual browser runs.
#
# Usage: tools/serve_webgpu.sh <build-dir> [port] [bind-address]
#        tools/serve_webgpu.sh --self-test [build-dir] [port] [bind-address]
#
# Runs a FOREGROUND python3 http.server with the MIME types the browser shell
# needs (.wasm -> application/wasm, .js/.mjs -> text/javascript, .data ->
# application/octet-stream; see docs/building.md, "Browser artifact deployment")
# and Cache-Control: no-store so rebuilt artifacts are never served stale. It
# binds to 127.0.0.1 by default because WebGPU requires a secure context and
# localhost qualifies without TLS.
#
# Serving blocks until interrupted and is meant for manual browser runs only;
# it is never invoked by an automated verify step. `--self-test` runs one
# request per MIME type through the handler over a socket pair (nothing listens
# on a port) and is safe for CI.
set -euo pipefail

usage() {
	echo "usage: $0 <build-dir> [port] [bind-address] | --self-test [build-dir] [port] [bind-address]" >&2
	exit 2
}

# A scalar rather than an array: expanding an empty array with "${arr[@]}"
# aborts under `set -u` on bash < 4.4 (macOS ships /bin/bash 3.2).
self_test=""
if [[ "${1:-}" == "--self-test" ]]; then
	self_test=--self-test
	shift
	[[ $# -le 3 ]] || usage
	build_dir="${1:-.}"
else
	[[ $# -ge 1 && $# -le 3 ]] || usage
	build_dir="$1"
	if [[ ! -d "$build_dir" ]]; then
		echo "$0: build directory '$build_dir' does not exist" >&2
		exit 1
	fi
	if [[ ! -f "$build_dir/index.html" ]]; then
		echo "$0: '$build_dir' has no index.html; point this at an Emscripten build directory (emcmake cmake ... -DDEVILUTIONX_WEBGPU=ON)" >&2
		exit 1
	fi
fi
port="${2:-8080}"
bind_address="${3:-127.0.0.1}"

if ! [[ "$port" =~ ^[0-9]+$ ]] || (( port < 1 || port > 65535 )); then
	echo "$0: invalid port '$port'" >&2
	exit 1
fi

exec python3 - ${self_test:+"$self_test"} "$build_dir" "$port" "$bind_address" <<'PY'
import functools
import http.client
import http.server
import os
import socket
import sys
import tempfile

MIME_TYPES = {
    ".wasm": "application/wasm",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".data": "application/octet-stream",
    ".html": "text/html; charset=utf-8",
    ".json": "application/json",
    ".mpq": "application/octet-stream",
}
CACHE_CONTROL = "no-store"


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **MIME_TYPES}

    def end_headers(self):
        self.send_header("Cache-Control", CACHE_CONTROL)
        super().end_headers()


class QuietHandler(Handler):
    def log_message(self, format, *args):
        pass


class NullServer:
    server_name = "serve_webgpu"
    server_port = 0


def request(directory, path):
    """Run one HEAD request through the handler over a socket pair (no port is bound)."""
    client, server = socket.socketpair()
    try:
        client.sendall(f"HEAD {path} HTTP/1.0\r\nHost: localhost\r\n\r\n".encode("ascii"))
        client.shutdown(socket.SHUT_WR)
        QuietHandler(server, ("127.0.0.1", 0), NullServer(), directory=directory)
        server.close()
        response = http.client.HTTPResponse(client, method="HEAD")
        response.begin()
        return response.status, response.getheader("Content-Type"), response.getheader("Cache-Control")
    finally:
        client.close()


def self_test(build_dir, port, bind_address):
    failures = 0
    with tempfile.TemporaryDirectory(prefix="serve_webgpu_self_test_") as directory:
        for extension, expected in MIME_TYPES.items():
            name = f"sample{extension}"
            with open(os.path.join(directory, name), "wb") as f:
                f.write(b"\0")
            status, content_type, cache_control = request(directory, f"/{name}")
            ok = status == 200 and content_type == expected and cache_control == CACHE_CONTROL
            print(f"{extension}\t{content_type}\t{'ok' if ok else 'FAIL'}")
            if not ok:
                failures += 1
                print(
                    f"serve_webgpu self-test: {name}: status {status}, Content-Type {content_type!r} "
                    f"(expected {expected!r}), Cache-Control {cache_control!r} (expected {CACHE_CONTROL!r})",
                    file=sys.stderr,
                )
        status, _, cache_control = request(directory, "/missing.wasm")
        if status != 404 or cache_control != CACHE_CONTROL:
            failures += 1
            print(f"serve_webgpu self-test: missing file returned status {status} with Cache-Control {cache_control!r}", file=sys.stderr)
    if failures:
        print(f"serve_webgpu self-test: {failures} failure(s)", file=sys.stderr)
        return 1
    print(f"serve_webgpu self-test: ok (would serve {os.path.abspath(build_dir)} on http://{bind_address}:{port}/)")
    return 0


def serve(build_dir, port, bind_address):
    handler = functools.partial(Handler, directory=build_dir)
    with http.server.ThreadingHTTPServer((bind_address, port), handler) as server:
        print(f"Serving {os.path.abspath(build_dir)} at http://{bind_address}:{port}/index.html (Ctrl-C to stop)")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


args = sys.argv[1:]
run_self_test = args[0] == "--self-test"
if run_self_test:
    args = args[1:]
build_dir, port, bind_address = args[0], int(args[1]), args[2]
sys.exit(self_test(build_dir, port, bind_address) if run_self_test else serve(build_dir, port, bind_address))
PY
