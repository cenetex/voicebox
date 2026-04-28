#!/usr/bin/env bash
# Serves the web/ dir with COOP/COEP headers
# (required later for SharedArrayBuffer when we add WASM threads).
# Override port: PORT=9090 ./serve.sh
set -euo pipefail
cd "$(dirname "$0")"
PORT="${PORT:-8765}"
exec python3 -c "
import http.server
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy',   'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()
http.server.test(HandlerClass=H, port=$PORT, bind='127.0.0.1')
"
