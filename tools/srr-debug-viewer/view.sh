#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Launcher — serves the viewer (with `data/` symlinks to multi-test runs)
# over http.server, opens the browser at the landing page.
#
# Usage: ./view.sh [port]
# To add more runs: ln -s /path/to/multi-test-XXX data/
set -euo pipefail

PORT="${1:-8765}"
VIEWER_DIR="$(realpath "$(dirname "$0")")"

if [[ ! -d "$VIEWER_DIR/data" ]]; then
  mkdir -p "$VIEWER_DIR/data"
  echo "created data/ — symlink multi-test dirs into it:"
  echo "  ln -s /path/to/multi-test-XXX $VIEWER_DIR/data/"
fi

URL="http://localhost:${PORT}/"
echo "viewer: $URL"
echo "(serving $VIEWER_DIR on port $PORT — Ctrl+C to stop)"
( command -v xdg-open >/dev/null && xdg-open "$URL" ) >/dev/null 2>&1 || true
# Serve with no-store headers so edited JS/CSS/data always reload — no more
# fighting the browser module cache (Ctrl+Shift+R / incognito not required).
cd "$VIEWER_DIR" && exec python3 -c '
import sys, http.server
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()
http.server.test(HandlerClass=H, port=int(sys.argv[1]), bind="0.0.0.0")
' "$PORT"
