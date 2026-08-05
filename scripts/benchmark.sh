#!/usr/bin/env bash
#
# Throughput and latency runs against the release build, one scenario per
# (response size, concurrency) pair.
#
# What this measures: the server, the loopback stack, and wrk, all on one
# machine, sharing the same CPUs. That is a useful comparison *between*
# the rows below, which is what it is for. It is not a number to quote as
# "this server does N requests per second": there is no network, no RTT,
# no packet loss, and the load generator is competing with the thing it
# is loading for cores. Treat the columns as relative, not absolute.
#
# Everything runs against http-server-release, never the sanitized build,
# see the RELEASE_CFLAGS comment in the Makefile for why measuring the
# instrumented binary would mostly measure the instrumentation.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${BENCH_PORT:-8080}"
HOST=127.0.0.1
DURATION="${BENCH_DURATION:-10s}"
CONNECTIONS=(1 10 50 200)

# "A few MB": big enough that the response cannot be one segment and the
# sendfile() path has to resume across several EPOLLOUT wakeups, small
# enough to stay in the page cache so the benchmark measures the server
# rather than the disk.
BIG_FILE_MB="${BENCH_BIG_FILE_MB:-8}"
BIG_FILE_NAME="benchmark-large.bin"
BIG_FILE="public/$BIG_FILE_NAME"

# name:path triples, evaluated in order. /hello.txt and the big file are
# the two the exercise asks for; / is the control that makes them
# readable, being the one route whose whole response is a single write()
# with no file body behind it. Without a row like that, a surprising
# number in the other two has nothing to be surprised against.
SCENARIOS=(
    "inline:/:in-memory route, no file"
    "small-file:/hello.txt:small static file via sendfile"
    "large-file:/$BIG_FILE_NAME:${BIG_FILE_MB}MiB static file via sendfile"
)

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS_DIR="benchmarks/results/$TIMESTAMP"
SUMMARY_TSV="$RESULTS_DIR/summary.tsv"
SUMMARY_TXT="$RESULTS_DIR/summary.txt"

SERVER_PID=""
SERVER_STOPPED=0

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: $1 is required but not installed" >&2
        exit 1
    }
}

# SIGTERM, then escalate. The server is supposed to shut down cleanly on
# it (that is layer 6's shutdown eventfd doing its job), so needing the
# SIGKILL below is itself a finding and gets reported as one.
stop_server() {
    [[ -n "$SERVER_PID" ]] || return 0
    [[ "$SERVER_STOPPED" -eq 0 ]] || return 0
    SERVER_STOPPED=1

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "warning: server was no longer running before shutdown" >&2
        return 0
    fi

    kill -TERM "$SERVER_PID" 2>/dev/null || true
    local waited=0
    while kill -0 "$SERVER_PID" 2>/dev/null && ((waited < 50)); do
        sleep 0.1
        waited=$((waited + 1))
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "warning: server ignored SIGTERM for 5s, killing it" >&2
        kill -KILL "$SERVER_PID" 2>/dev/null || true
    fi

    local status=0
    wait "$SERVER_PID" 2>/dev/null || status=$?
    if [[ "$status" -eq 0 ]]; then
        echo "server shut down cleanly (exit 0)"
    else
        echo "warning: server exited with status $status, see $RESULTS_DIR/server.log" >&2
    fi
}

trap stop_server EXIT INT TERM

require wrk
require curl

echo "==> building the release binary"
make release

if [[ ! -f "$BIG_FILE" ]] || [[ "$(stat -c %s "$BIG_FILE")" -ne $((BIG_FILE_MB * 1024 * 1024)) ]]; then
    echo "==> generating $BIG_FILE (${BIG_FILE_MB}MiB)"
    # Generated rather than committed: it is a benchmark fixture, and
    # several MB of random bytes have no business in git history. Random
    # rather than zeros so nothing in the stack can shortcut it.
    head -c "$((BIG_FILE_MB * 1024 * 1024))" /dev/urandom >"$BIG_FILE"
fi

mkdir -p "$RESULTS_DIR"

if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
    exec 3>&-
    echo "error: something is already listening on $HOST:$PORT" >&2
    exit 1
fi

# No HTTP_SERVER_WORKERS: the pool then detects the online CPU count and
# runs one worker per core, which is the configuration being measured.
echo "==> starting http-server-release on port $PORT"
./http-server-release >"$RESULTS_DIR/server.log" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
        exec 3>&-
        ready=1
        break
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
    echo "error: server never started listening, see $RESULTS_DIR/server.log" >&2
    cat "$RESULTS_DIR/server.log" >&2
    exit 1
fi

# wrk reports a 404 as a perfectly good request, so a typo in a path
# would show up as an excellent result rather than as an error. Check
# every URL once, up front, before spending two minutes measuring.
echo "==> checking scenario URLs"
for scenario in "${SCENARIOS[@]}"; do
    path="$(cut -d: -f2 <<<"$scenario")"
    # The trailing newline in -w is load bearing: read returns non-zero
    # at EOF without one, which under `set -e` would end the script here.
    read -r code size < <(curl -sS -o /dev/null -w '%{http_code} %{size_download}\n' "http://$HOST:$PORT$path")
    if [[ "$code" != "200" ]]; then
        echo "error: $path answered $code, refusing to benchmark it" >&2
        exit 1
    fi
    echo "    $path -> $code, $size bytes"
done

# Everything needed to make sense of these numbers six months from now,
# or to know why two runs disagree.
{
    echo "timestamp:      $TIMESTAMP"
    echo "git commit:     $(git rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')"
    echo "git dirty:      $(git diff --quiet 2>/dev/null && echo no || echo yes)"
    echo "host:           $(uname -srm)"
    echo "cpus online:    $(nproc)"
    echo "compiler:       $(${CC:-gcc} --version | head -1)"
    # Read out of the Makefile rather than from `make -n`, which prints
    # nothing useful once the binary is already up to date.
    echo "release cflags: $(sed -n 's/^RELEASE_CFLAGS = //p' Makefile)"
    echo "wrk:            $(wrk --version 2>&1 | head -1)"
    echo "duration:       $DURATION per run"
    echo "big file:       ${BIG_FILE_MB}MiB"
    echo "server banner:  $(head -1 "$RESULTS_DIR/server.log")"
} >"$RESULTS_DIR/environment.txt"

printf 'scenario\tpath\tconnections\tthreads\trequests_per_sec\ttransfer_mb_per_sec\tavg_latency_ms\tp50_latency_ms\tp99_latency_ms\terrors\n' >"$SUMMARY_TSV"

# wrk's own thread count. Capped at half the cores so the load generator
# and the server are not fighting over all 8 of them, and never more
# threads than connections (wrk refuses that outright).
cpus="$(nproc)"
max_threads=$((cpus / 2))
((max_threads > 0)) || max_threads=1

parse_run() {
    # wrk prints durations with a unit suffix that changes with the
    # magnitude (us/ms/s/m), and sizes likewise (KB/MB/GB), so every
    # field has to be normalized before rows can be compared. Anything
    # missing becomes 0 rather than an empty column.
    awk -v scenario="$1" -v path="$2" -v conns="$3" -v threads="$4" '
        function to_ms(v) {
            if (v ~ /us$/) return substr(v, 1, length(v) - 2) / 1000
            if (v ~ /ms$/) return substr(v, 1, length(v) - 2) + 0
            if (v ~ /m$/)  return substr(v, 1, length(v) - 1) * 60000
            if (v ~ /h$/)  return substr(v, 1, length(v) - 1) * 3600000
            if (v ~ /s$/)  return substr(v, 1, length(v) - 1) * 1000
            return v + 0
        }
        function to_mb(v) {
            if (v ~ /KB$/) return substr(v, 1, length(v) - 2) / 1024
            if (v ~ /MB$/) return substr(v, 1, length(v) - 2) + 0
            if (v ~ /GB$/) return substr(v, 1, length(v) - 2) * 1024
            if (v ~ /B$/)  return substr(v, 1, length(v) - 1) / 1048576
            return v + 0
        }
        /^ *Latency  / && !avg_seen { avg = to_ms($2); avg_seen = 1 }
        /^ *50%/  { p50 = to_ms($2) }
        /^ *99%/  { p99 = to_ms($2) }
        /^Requests\/sec:/ { rps = $2 + 0 }
        /^Transfer\/sec:/ { mbps = to_mb($2) }
        /^ *Socket errors:/ {
            # "Socket errors: connect 0, read 3, write 0, timeout 12"
            gsub(/,/, "")
            for (i = 3; i < NF; i += 2) if ($(i + 1) + 0 > 0) errors = errors $i "=" $(i + 1) " "
        }
        /^ *Non-2xx or 3xx responses:/ { errors = errors "non-2xx=" $NF " " }
        END {
            sub(/ $/, "", errors)
            if (errors == "") errors = "-"
            printf "%s\t%s\t%s\t%s\t%.0f\t%.1f\t%.2f\t%.2f\t%.2f\t%s\n",
                   scenario, path, conns, threads, rps, mbps, avg, p50, p99, errors
        }
    ' "$5"
}

echo "==> running ${#SCENARIOS[@]} scenarios x ${#CONNECTIONS[@]} concurrency levels, $DURATION each"
for scenario in "${SCENARIOS[@]}"; do
    name="$(cut -d: -f1 <<<"$scenario")"
    path="$(cut -d: -f2 <<<"$scenario")"
    label="$(cut -d: -f3 <<<"$scenario")"
    url="http://$HOST:$PORT$path"

    echo
    echo "--- $name ($label)"

    # One short unrecorded run first: the first requests of a scenario
    # pay for the page cache filling and for connections being
    # established, and folding that into a 10s average would penalize
    # whichever scenario happens to go first.
    wrk -t1 -c10 -d2s --timeout 10s "$url" >"$RESULTS_DIR/$name-warmup.txt" 2>&1 || true

    for conns in "${CONNECTIONS[@]}"; do
        threads=$((conns < max_threads ? conns : max_threads))
        raw="$RESULTS_DIR/$name-c$conns.txt"

        printf '    %-4s connections, %s threads ... ' "$conns" "$threads"
        wrk -t"$threads" -c"$conns" -d"$DURATION" --latency --timeout 10s "$url" >"$raw" 2>&1

        row="$(parse_run "$name" "$path" "$conns" "$threads" "$raw")"
        printf '%s req/s\n' "$(cut -f5 <<<"$row")"
        printf '%s\n' "$row" >>"$SUMMARY_TSV"
    done
done

echo
stop_server

{
    echo "benchmark $TIMESTAMP  ($DURATION per run, $(nproc) cpus, one worker per cpu)"
    echo
    awk -F'\t' '
        NR == 1 {
            printf "%-12s %-22s %5s %4s %12s %11s %10s %10s %10s  %s\n",
                   "SCENARIO", "PATH", "CONN", "THR", "REQ/S", "MB/S", "AVG ms", "P50 ms", "P99 ms", "ERRORS"
            next
        }
        {
            printf "%-12s %-22s %5s %4s %12s %11s %10s %10s %10s  %s\n",
                   $1, $2, $3, $4, $5, $6, $7, $8, $9, $10
        }
    ' "$SUMMARY_TSV"
    echo
    echo "raw wrk output and environment.txt: $RESULTS_DIR/"
} | tee "$SUMMARY_TXT"
