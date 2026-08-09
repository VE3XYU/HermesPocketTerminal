#!/usr/bin/env bash
# Starts bridge --mock for host integration tests. Usage: run-mock.sh [port]
set -euo pipefail
PORT="${1:-18787}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
"$REPO/bridge/.venv/bin/htp-bridge" --mock --port "$PORT" &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
  if curl -sf -H "Authorization: Bearer x" "http://127.0.0.1:$PORT/htp/v1/dashboard" >/dev/null; then
    echo "mock ready on :$PORT"
    wait $MOCK_PID
    exit 0
  fi
  sleep 0.2
done
echo "mock failed to start" >&2
exit 1
