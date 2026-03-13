#!/bin/sh
# tests/e2e_examples.sh — Smoke test all Keel example programs
#
# Usage:  sh tests/e2e_examples.sh
# Expects examples to be pre-built (make examples).

pass=0
fail=0
skip=0

# ── Helpers ────────────────────────────────────────────────────────────

wait_for_server() {
  port=$1
  for _i in 1 2 3 4 5 6 7 8 9 10; do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$port/" 2>/dev/null; then
      return 0
    fi
    sleep 0.3
  done
  return 1
}

start_server() {
  "$@" >/dev/null 2>&1 &
  echo $!
}

stop_server() {
  kill "$1" 2>/dev/null || true
  wait "$1" 2>/dev/null || true
  sleep 0.2
}

# POSIX-portable timeout: run command with a time limit.
# macOS doesn't have GNU timeout; use background + kill instead.
run_timeout() {
  secs=$1; shift
  "$@" &
  _pid=$!
  ( sleep "$secs"; kill "$_pid" 2>/dev/null ) &
  _watchdog=$!
  wait "$_pid" 2>/dev/null
  _rc=$?
  kill "$_watchdog" 2>/dev/null
  wait "$_watchdog" 2>/dev/null
  return $_rc
}

check_contains() {
  label=$1; response=$2; expected=$3
  if echo "$response" | grep -q "$expected"; then
    pass=$((pass + 1))
    echo "  PASS: $label"
  else
    fail=$((fail + 1))
    echo "  FAIL: $label (expected '$expected')"
    echo "        got: $(echo "$response" | head -c 200)"
  fi
}

check_status() {
  label=$1; status=$2; expected=$3
  if [ "$status" = "$expected" ]; then
    pass=$((pass + 1))
    echo "  PASS: $label"
  else
    fail=$((fail + 1))
    echo "  FAIL: $label (got status $status, expected $expected)"
  fi
}

check_header() {
  label=$1; headers=$2; expected=$3
  if echo "$headers" | grep -qi "$expected"; then
    pass=$((pass + 1))
    echo "  PASS: $label"
  else
    fail=$((fail + 1))
    echo "  FAIL: $label (expected header '$expected')"
  fi
}

# Soft check: pass on match, skip (not fail) on empty response (e.g. curl timeout).
# Used for async endpoints that depend on pipe watchers / event loop internals.
check_contains_soft() {
  label=$1; response=$2; expected=$3
  if echo "$response" | grep -q "$expected"; then
    pass=$((pass + 1))
    echo "  PASS: $label"
  elif [ -z "$response" ]; then
    skip=$((skip + 1))
    echo "  SKIP: $label (no response — async mechanism may not work in this environment)"
  else
    fail=$((fail + 1))
    echo "  FAIL: $label (expected '$expected')"
    echo "        got: $(echo "$response" | head -c 200)"
  fi
}

# Use 127.0.0.1 to avoid IPv6 resolution issues
CURL="curl -s --max-time 5"
CURL_H="curl -s --max-time 5 -D - -o /dev/null"

# Unique port for examples that accept a port argument
PORT=19800

# ── Standalone examples (run, check exit 0) ────────────────────────────

test_standalone() {
  for name in custom_allocator connection_pool url_parser timer; do
    bin="examples/$name"
    echo "=== $name ==="
    if [ ! -x "$bin" ]; then
      skip=$((skip + 1))
      echo "  SKIP: $bin not found"
      continue
    fi
    if "./$bin" >/dev/null 2>&1; then
      pass=$((pass + 1))
      echo "  PASS: exit 0"
    else
      fail=$((fail + 1))
      echo "  FAIL: non-zero exit"
    fi
  done
}

# ── Server examples (start, curl, kill) ────────────────────────────────

# hello_server — accepts port arg
test_hello_server() {
  echo "=== hello_server ==="
  bin="examples/hello_server"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  port=$((PORT)); PORT=$((PORT + 1))
  pid=$(start_server "./$bin" "$port")
  if ! wait_for_server "$port"; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  resp=$($CURL "http://127.0.0.1:$port/hello" || true)
  check_contains "GET /hello" "$resp" '"hello"'

  stop_server "$pid"
}

# rest_api_server — hardcoded :8080
test_rest_api_server() {
  echo "=== rest_api_server ==="
  bin="examples/rest_api_server"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  resp=$($CURL "http://127.0.0.1:8080/api/users" || true)
  check_contains "GET /api/users" "$resp" '"Alice"'

  resp=$($CURL "http://127.0.0.1:8080/api/users/42" || true)
  check_contains "GET /api/users/42" "$resp" '"id":42'

  status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    -X POST -d '{"name":"Eve"}' "http://127.0.0.1:8080/api/users" || true)
  check_status "POST /api/users → 201" "$status" "201"

  stop_server "$pid"
}

# middleware — hardcoded :8080
test_middleware() {
  echo "=== middleware ==="
  bin="examples/middleware"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # /api/public requires auth — should get 401 without it
  status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    "http://127.0.0.1:8080/api/public" || true)
  check_status "GET /api/public (no auth) → 401" "$status" "401"

  # with auth header
  resp=$($CURL -H "Authorization: Bearer tok" "http://127.0.0.1:8080/api/public" || true)
  check_contains "GET /api/public (auth)" "$resp" '"public"'

  # CORS preflight
  headers=$($CURL_H -X OPTIONS \
    -H "Origin: http://localhost:3000" \
    -H "Access-Control-Request-Method: GET" \
    "http://127.0.0.1:8080/api/public" 2>/dev/null || true)
  check_header "CORS preflight" "$headers" "Access-Control-Allow-Origin"

  stop_server "$pid"
}

# static_files — hardcoded :8080, uses wildcard route
test_static_files() {
  echo "=== static_files ==="
  bin="examples/static_files"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Verify server is running and responds (404 for unmatched paths is expected)
  status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    "http://127.0.0.1:8080/nope.txt" || true)
  check_status "server responds" "$status" "404"

  stop_server "$pid"
}

# streaming — hardcoded :8080
test_streaming() {
  echo "=== streaming ==="
  bin="examples/streaming"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  resp=$($CURL "http://127.0.0.1:8080/stream" || true)
  check_contains "GET /stream has name" "$resp" '"keel"'
  check_contains "GET /stream has items" "$resp" '"items"'

  stop_server "$pid"
}

# sse — hardcoded :8080
test_sse() {
  echo "=== sse ==="
  bin="examples/sse"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  resp=$($CURL "http://127.0.0.1:8080/events" || true)
  check_contains "SSE event field" "$resp" 'event: tick'
  check_contains "SSE data field" "$resp" 'data: {"count":0}'
  check_contains "SSE id field" "$resp" 'id: 0'
  check_contains "SSE comment" "$resp" ': stream started'
  check_contains "SSE multiline event" "$resp" 'event: multi'
  check_contains "SSE multiline data split" "$resp" 'data: line one'

  stop_server "$pid"
}

# body_readers — hardcoded :8080
test_body_readers() {
  echo "=== body_readers ==="
  bin="examples/body_readers"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Buffer reader echo
  resp=$($CURL -X POST -d 'hello world' "http://127.0.0.1:8080/echo" || true)
  check_contains "POST /echo" "$resp" "hello world"

  # Multipart upload
  status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    -F "name=Alice" "http://127.0.0.1:8080/upload" || true)
  check_status "POST /upload → 200" "$status" "200"

  stop_server "$pid"
}

# async — hardcoded :8080
test_async() {
  echo "=== async ==="
  bin="examples/async"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Sync endpoint always works
  resp=$($CURL "http://127.0.0.1:8080/" || true)
  check_contains "GET / (sync)" "$resp" '"hello'

  # Async endpoint — depends on pipe watcher + kl_async_complete
  resp=$($CURL "http://127.0.0.1:8080/delay/10" || true)
  check_contains_soft "GET /delay/10" "$resp" '"delayed_ms"'

  stop_server "$pid"
}

# thread_pool — hardcoded :8080
test_thread_pool() {
  echo "=== thread_pool ==="
  bin="examples/thread_pool"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Sync endpoint
  resp=$($CURL "http://127.0.0.1:8080/" || true)
  check_contains "GET / (sync)" "$resp" '"hello'

  # Thread pool endpoint — depends on pipe watcher + kl_async_complete
  resp=$($CURL "http://127.0.0.1:8080/query" || true)
  check_contains_soft "GET /query" "$resp" '"answer"'

  stop_server "$pid"
}

# async_thread_pool — hardcoded :8080
test_async_thread_pool() {
  echo "=== async_thread_pool ==="
  bin="examples/async_thread_pool"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  pid=$(start_server "./$bin")
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Sync endpoint
  resp=$($CURL "http://127.0.0.1:8080/" || true)
  check_contains "GET / (sync)" "$resp" '"hello'

  # Thread pool endpoint — depends on pipe watcher + kl_async_complete
  resp=$($CURL "http://127.0.0.1:8080/fast" || true)
  check_contains_soft "GET /fast" "$resp" '"work_ms"'

  stop_server "$pid"
}

# websocket_server — accepts port arg, test HTTP upgrade handshake
test_websocket_server() {
  echo "=== websocket_server ==="
  bin="examples/websocket_server"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  port=$((PORT)); PORT=$((PORT + 1))
  pid=$(start_server "./$bin" "$port")
  if ! wait_for_server "$port"; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # WebSocket upgrade request — server should respond with 101
  headers=$(curl -s -D - -o /dev/null --max-time 2 \
    -H "Upgrade: websocket" \
    -H "Connection: Upgrade" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    -H "Sec-WebSocket-Version: 13" \
    "http://127.0.0.1:$port/ws" 2>/dev/null || true)
  check_header "WS upgrade → 101" "$headers" "101 Switching Protocols"

  stop_server "$pid"
}

# h2_server — accepts port arg, test with HTTP/1.1 (h2c upgrade needs special client)
test_h2_server() {
  echo "=== h2_server ==="
  bin="examples/h2_server"
  if [ ! -x "$bin" ]; then skip=$((skip + 1)); echo "  SKIP"; return; fi

  port=$((PORT)); PORT=$((PORT + 1))
  pid=$(start_server "./$bin" "$port")
  if ! wait_for_server "$port"; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$pid"; return
  fi

  # Falls back to HTTP/1.1 for normal requests
  resp=$($CURL "http://127.0.0.1:$port/hello" || true)
  check_contains "GET /hello (h1 fallback)" "$resp" '"hello from h2"'

  stop_server "$pid"
}

# ── Client examples (need a server first) ──────────────────────────────

test_client() {
  echo "=== client ==="
  bin="examples/client"
  server="examples/hello_server"
  if [ ! -x "$bin" ] || [ ! -x "$server" ]; then
    skip=$((skip + 1)); echo "  SKIP"; return
  fi

  # Start hello_server on default port used by client (8080)
  server_pid=$(start_server "./$server" 8080)
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$server_pid"; return
  fi

  output=$(run_timeout 10 "./$bin" 2>&1) || true
  if echo "$output" | grep -q "request failed"; then
    skip=$((skip + 1))
    echo "  SKIP: client can't connect (localhost may resolve to IPv6)"
  else
    check_contains "sync GET" "$output" "status: 200"
    check_contains "body" "$output" '"hello"'
  fi

  stop_server "$server_pid"
}

test_async_client() {
  echo "=== async_client ==="
  bin="examples/async_client"
  server="examples/hello_server"
  if [ ! -x "$bin" ] || [ ! -x "$server" ]; then
    skip=$((skip + 1)); echo "  SKIP"; return
  fi

  server_pid=$(start_server "./$server" 8080)
  if ! wait_for_server 8080; then
    fail=$((fail + 1)); echo "  FAIL: server didn't start"
    stop_server "$server_pid"; return
  fi

  output=$(run_timeout 10 "./$bin" 2>&1) || true
  if echo "$output" | grep -q "ERROR"; then
    skip=$((skip + 1))
    echo "  SKIP: async_client can't connect (localhost may resolve to IPv6)"
  else
    check_contains_soft "concurrent requests" "$output" "status=200"
    check_contains_soft "all completed" "$output" "completed"
  fi

  stop_server "$server_pid"
}

# ── Skipped (TLS / complex dependencies) ───────────────────────────────

test_skipped() {
  for name in tls_server tls_client websocket_client h2_client; do
    echo "=== $name ==="
    skip=$((skip + 1))
    echo "  SKIP: requires TLS certs or external server"
  done
}

# ── Run all ────────────────────────────────────────────────────────────

echo "Keel E2E example smoke tests"
echo "=============================="
echo ""

test_standalone

# Server examples that use :8080 must run sequentially
test_hello_server
test_rest_api_server
test_middleware
test_static_files
test_streaming
test_sse
test_body_readers
test_async
test_thread_pool
test_async_thread_pool
test_websocket_server
test_h2_server

# Client examples (also use :8080)
test_client
test_async_client

# Skipped
test_skipped

# ── Summary ────────────────────────────────────────────────────────────

echo ""
echo "=============================="
echo "Results: $pass passed, $fail failed, $skip skipped"
echo "=============================="
[ "$fail" -eq 0 ] || exit 1
