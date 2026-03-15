#!/bin/sh
# KEEL multi-endpoint benchmark — build + run 4 wrk benchmarks
#
# Usage:
#   make bench                  # default: 4 threads, 100 connections, 10s
#   JSON=1 make bench           # JSON output via bench_json.lua
#   THREADS=8 CONNECTIONS=200 DURATION=15s make bench

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=${PORT:-9090}
THREADS=${THREADS:-4}
CONNECTIONS=${CONNECTIONS:-100}
DURATION=${DURATION:-10s}
BASE="http://localhost:$PORT"

# Build bench server if needed
if [ ! -f "$SCRIPT_DIR/bench_server" ]; then
    echo "Building bench_server..."
    make -C "$SCRIPT_DIR/.." bench/bench_server 2>/dev/null
fi

# Check for wrk
if ! command -v wrk >/dev/null 2>&1; then
    echo "wrk not found. Install with: brew install wrk  (macOS) or apt install wrk (Linux)"
    exit 1
fi

# Start server
"$SCRIPT_DIR/bench_server" "$PORT" 2>/dev/null &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT

# Wait for server to be ready
for i in 1 2 3 4 5; do
    if curl -s "$BASE/hello" >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! curl -s "$BASE/hello" >/dev/null 2>&1; then
    echo "Server failed to start"
    exit 1
fi

# Detect system info
OS=$(uname -s)
OS_VER=$(uname -r)
ARCH=$(uname -m)
if [ "$OS" = "Darwin" ]; then
    CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
elif [ -f /proc/cpuinfo ]; then
    CPU=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //' || echo "unknown")
else
    CPU="unknown"
fi

# Detect event backend
if [ "$OS" = "Darwin" ]; then
    BACKEND="kqueue"
elif [ "$OS" = "Linux" ]; then
    BACKEND="epoll"
fi
[ -n "$KEEL_BACKEND" ] && BACKEND="$KEEL_BACKEND"

# wrk options
WRK_OPTS="-t$THREADS -c$CONNECTIONS -d$DURATION --latency"
if [ -n "$JSON" ]; then
    WRK_JSON="-s $SCRIPT_DIR/bench_json.lua"
fi

# Warmup
wrk -t2 -c10 -d2s "$BASE/hello" >/dev/null 2>&1

if [ -n "$JSON" ]; then
    # JSON mode — array of 4 results
    echo "["

    printf '{"endpoint":"GET /hello","label":"baseline","results":'
    wrk $WRK_OPTS $WRK_JSON "$BASE/hello"
    printf '},'

    printf '{"endpoint":"GET /users/42","label":"route_params","results":'
    wrk $WRK_OPTS $WRK_JSON "$BASE/users/42"
    printf '},'

    printf '{"endpoint":"GET /mw/hello","label":"middleware","results":'
    wrk $WRK_OPTS $WRK_JSON "$BASE/mw/hello"
    printf '},'

    printf '{"endpoint":"POST /echo","label":"body_reading","results":'
    wrk $WRK_OPTS $WRK_JSON -s "$SCRIPT_DIR/post_body.lua" "$BASE/echo"
    printf '}'

    echo "]"
else
    # Human-readable mode
    echo "=== KEEL Benchmark ==="
    echo "  os:           $OS $OS_VER ($ARCH)"
    echo "  cpu:          $CPU"
    echo "  backend:      $BACKEND"
    echo "  threads:      $THREADS"
    echo "  connections:  $CONNECTIONS"
    echo "  duration:     $DURATION"
    echo ""

    echo "--- GET /hello (baseline) ---"
    wrk $WRK_OPTS "$BASE/hello"
    echo ""

    echo "--- GET /users/42 (route params) ---"
    wrk $WRK_OPTS "$BASE/users/42"
    echo ""

    echo "--- GET /mw/hello (middleware chain) ---"
    wrk $WRK_OPTS "$BASE/mw/hello"
    echo ""

    echo "--- POST /echo (body reading) ---"
    wrk $WRK_OPTS -s "$SCRIPT_DIR/post_body.lua" "$BASE/echo"
fi
