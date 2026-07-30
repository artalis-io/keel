#!/bin/sh
# bench_compare.sh — compare KEEL's Linux event backends on ONE machine (PAL 8f step 4).
#
# Builds the library + bench_server for each of epoll (readiness, default), io_uring
# readiness-adapted (BACKEND=iouring), and io_uring completion-native (BACKEND=iouringcomp),
# then runs the SAME wrk workload against each and prints a comparison. Running all three
# back-to-back on one host keeps the comparison RELATIVE-fair (same CPU, same noise floor) —
# absolute numbers from a shared CI VM are not publishable, but the ordering (does the
# completion backend match/beat readiness?) is the step-4 question.
#
# Usage:  bench/bench_compare.sh          (needs wrk, curl, liburing-dev, gcc)
#         DURATION=8s CONNECTIONS=100 THREADS=4 bench/bench_compare.sh
set -e

cd "$(dirname "$0")/.."
PORT=${PORT:-9099}
DUR=${DURATION:-8s}
CONN=${CONNECTIONS:-100}
THR=${THREADS:-4}
CFLAGS="-std=c11 -O2 -Iinclude -Ivendor/llhttp"

command -v wrk  >/dev/null 2>&1 || { echo "wrk not found (apt install wrk)"; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "curl not found"; exit 1; }

# results accumulate here: "<label>\t<endpoint>\t<req/s>\t<p99>"
RESULTS=$(mktemp)
trap 'rm -f "$RESULTS"' EXIT

bench_backend() {
    make_args="$1"; label="$2"; extra_cc="$3"
    echo ">>> building: $label  ($make_args)" >&2
    make clean >/dev/null 2>&1
    make $make_args >/dev/null 2>&1
    # shellcheck disable=SC2086
    cc $CFLAGS $extra_cc -o bench/bench_server bench/bench_server.c -L. -lkeel -lpthread \
        $(echo "$make_args" | grep -q iouring && echo -luring)

    ./bench/bench_server "$PORT" >/dev/null 2>&1 &
    pid=$!
    ready=0
    for _ in $(seq 1 40); do
        if curl -s "http://localhost:$PORT/hello" >/dev/null 2>&1; then ready=1; break; fi
        sleep 0.2
    done
    if [ "$ready" = 0 ]; then echo "$label: server failed to start" >&2; kill $pid 2>/dev/null || true; return; fi

    wrk -t2 -c10 -d2s "http://localhost:$PORT/hello" >/dev/null 2>&1   # warmup

    for ep in "/hello:GET" "/echo:POST"; do
        path=${ep%:*}; method=${ep#*:}
        if [ "$method" = POST ]; then
            out=$(wrk -t"$THR" -c"$CONN" -d"$DUR" --latency -s bench/post_body.lua "http://localhost:$PORT$path")
        else
            out=$(wrk -t"$THR" -c"$CONN" -d"$DUR" --latency "http://localhost:$PORT$path")
        fi
        rps=$(echo "$out" | awk '/Requests\/sec/{print $2}')
        p99=$(echo "$out" | awk '/^ *99%/{print $2}')
        printf '%s\t%s %s\t%s\t%s\n' "$label" "$method" "$path" "${rps:-?}" "${p99:-?}" >>"$RESULTS"
    done

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}

bench_backend ""                   "epoll (readiness)"        ""
bench_backend "BACKEND=iouring"    "io_uring (readiness)"     ""
bench_backend "BACKEND=iouringcomp" "io_uring (completion)"   "-DKEEL_BENCH_OVERLAPPED"

echo ""
echo "=== KEEL backend comparison (relative; single host) ==="
[ -f /proc/cpuinfo ] && echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
echo "wrk: -t$THR -c$CONN -d$DUR"
echo ""
printf '%-24s %-12s %14s %10s\n' "backend" "endpoint" "req/s" "p99"
printf '%-24s %-12s %14s %10s\n' "------------------------" "------------" "--------------" "----------"
while IFS="$(printf '\t')" read -r label ep rps p99; do
    printf '%-24s %-12s %14s %10s\n' "$label" "$ep" "$rps" "$p99"
done <"$RESULTS"
