#!/bin/sh
# KEEL benchmark — runs wrk against the hello example server

PORT=${PORT:-9090}
THREADS=${THREADS:-4}
CONNECTIONS=${CONNECTIONS:-100}
DURATION=${DURATION:-10s}
URL="http://localhost:$PORT/hello"

# Build if needed
if [ ! -f examples/hello_server ]; then
    echo "Building..."
    make examples 2>/dev/null
fi

# Check for wrk
if ! command -v wrk >/dev/null 2>&1; then
    echo "wrk not found. Install with: brew install wrk"
    exit 1
fi

# Start server
./examples/hello_server "$PORT" 2>/dev/null &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT

# Wait for server to be ready
for i in 1 2 3 4 5; do
    if curl -s "$URL" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! curl -s "$URL" >/dev/null 2>&1; then
    echo "Server failed to start"
    exit 1
fi

echo "=== KEEL Benchmark ==="
echo "  threads:      $THREADS"
echo "  connections:  $CONNECTIONS"
echo "  duration:     $DURATION"
echo "  url:          $URL"
echo ""

# Warmup
wrk -t2 -c10 -d2s "$URL" >/dev/null 2>&1

# Benchmark
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL"
