#!/usr/bin/env bash
# common.sh - shared helpers for the test scripts (sourced, not executed)
#
# Environment knobs:
#   PORT      TCP port to use (default 9090)
#   HOST      address (default 127.0.0.1)
#   BIN_DIR   directory containing server/client (default build/bin)
#   EVIDENCE  directory for logs (default docs/evidence)

set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PORT="${PORT:-9090}"
HOST="${HOST:-127.0.0.1}"
BIN_DIR="${BIN_DIR:-$ROOT_DIR/build/bin}"
EVIDENCE="${EVIDENCE:-$ROOT_DIR/docs/evidence}"
mkdir -p "$EVIDENCE"

SERVER_BIN="$BIN_DIR/server"
CLIENT_BIN="$BIN_DIR/client"

PASS_COUNT=0
FAIL_COUNT=0
SERVER_PID=""
SERVER_LOG=""

ts() { date '+%Y-%m-%d %H:%M:%S'; }

# log "msg" -> stdout and $LOGFILE (if set)
log() {
    local line="[$(ts)] $*"
    echo "$line"
    if [ -n "${LOGFILE:-}" ]; then echo "$line" >> "$LOGFILE"; fi
}

# section "title"
section() {
    log ""
    log "=== $* ==="
}

pass() { PASS_COUNT=$((PASS_COUNT + 1)); log "PASS: $*"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); log "FAIL: $*"; }

# check "description" <condition-exit-code>
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}

# expect_match "description" "regex" "actual"
expect_match() {
    local desc="$1" re="$2" actual="$3"
    if [[ "$actual" =~ $re ]]; then
        pass "$desc -> '$actual'"
    else
        fail "$desc -> expected /$re/, got '$actual'"
    fi
}

port_in_use() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk '{print $4}' | grep -q ":${PORT}\$"
    else
        (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null
    fi
}

# wait_for_port [timeout_sec]
wait_for_port() {
    local tries=$(( ${1:-5} * 10 ))
    while [ "$tries" -gt 0 ]; do
        if port_in_use; then return 0; fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

# start_server <logfile> [server args...]
start_server() {
    SERVER_LOG="$1"; shift
    : > "$SERVER_LOG"
    "$SERVER_BIN" --host "$HOST" --port "$PORT" "$@" 2>> "$SERVER_LOG" &
    SERVER_PID=$!
    if ! wait_for_port 5; then
        log "ERROR: server did not start listening on $HOST:$PORT"
        cat "$SERVER_LOG"
        return 1
    fi
    log "server started pid=$SERVER_PID args='$*' log=$SERVER_LOG"
    return 0
}

stop_server() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        # wait up to 5 s
        local i=0
        while kill -0 "$SERVER_PID" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i + 1))
        done
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -KILL "$SERVER_PID" 2>/dev/null || true
        fi
        wait "$SERVER_PID" 2>/dev/null || true
        log "server pid=$SERVER_PID stopped"
    fi
    SERVER_PID=""
}

# Kill any stray children of a (possibly already dead) server.
kill_children_of() {
    local ppid="$1"
    local kids
    kids="$(pgrep -P "$ppid" 2>/dev/null || true)"
    if [ -n "$kids" ]; then
        # shellcheck disable=SC2086
        kill -KILL $kids 2>/dev/null || true
    fi
}

cleanup_common() {
    if [ -n "$SERVER_PID" ]; then
        kill_children_of "$SERVER_PID"
        stop_server
    fi
    pkill -P $$ 2>/dev/null || true
}
trap cleanup_common EXIT

# ps_children <ppid> : pid ppid stat etimes cmd of server children
ps_children() {
    ps -o pid,ppid,stat,etimes,cmd --ppid "$1" 2>/dev/null || true
}

# client_cmd "<CMD>" [extra client args] -> prints response (stdout only)
client_cmd() {
    local cmd="$1"; shift
    "$CLIENT_BIN" --host "$HOST" --port "$PORT" --command "$cmd" "$@" 2>/dev/null
}

summary() {
    log ""
    log "=== SUMMARY: PASS=$PASS_COUNT FAIL=$FAIL_COUNT ==="
    [ "$FAIL_COUNT" -eq 0 ]
}

require_binaries() {
    if [ ! -x "$SERVER_BIN" ] || [ ! -x "$CLIENT_BIN" ]; then
        echo "ERROR: binaries not found in $BIN_DIR (run make first)" >&2
        exit 1
    fi
}
