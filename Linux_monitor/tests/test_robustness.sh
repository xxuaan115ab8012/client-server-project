#!/usr/bin/env bash
# test_robustness.sh - SPEC 12: three comparative robustness experiments
#
#   RB-DEMO-01  unknown command then PING on the same connection
#   RB-DEMO-02  client connects and sends nothing (--idle 15)
#   RB-DEMO-03  10 sequential clients -> zombie check with ps
#
# Usage: tests/test_robustness.sh --mode robust     (uses build/bin)
#        tests/test_robustness.sh --mode baseline   (uses build/baseline/bin)
# Evidence: docs/evidence/robustness_<mode>.log (+ _server.log)
#
# In robust mode every check is an acceptance criterion (PASS/FAIL).
# In baseline mode the script records the observed behaviour; the
# "expected baseline weakness" observations are reported as OBSERVED.

MODE="robust"
while [ $# -gt 0 ]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --mode=*) MODE="${1#--mode=}"; shift ;;
        -h|--help)
            sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
case "$MODE" in robust|baseline) ;; *) echo "mode must be robust|baseline" >&2; exit 2 ;; esac

if [ "$MODE" = "baseline" ] && [ -z "${BIN_DIR:-}" ]; then
    BIN_DIR="$(cd "$(dirname "$0")/.." && pwd)/build/baseline/bin"
    export BIN_DIR
fi

source "$(dirname "$0")/common.sh"
require_binaries

IDLE_SEC="${IDLE_SEC:-15}"
LOGFILE="$EVIDENCE/robustness_${MODE}.log"
: > "$LOGFILE"
SRV_LOG="$EVIDENCE/robustness_${MODE}_server.log"

# observe "description" <cmd...> : baseline-only expectation, never fails
observe() {
    local desc="$1"; shift
    if "$@"; then log "OBSERVED (baseline weakness reproduced): $desc"
    else log "OBSERVED (baseline weakness NOT reproduced): $desc"; fi
}

log "test_robustness.sh start  mode=$MODE bin=$BIN_DIR port=$PORT"
log "binary identity: $(ls -l "$SERVER_BIN" | awk '{print $5, $6, $7, $8, $9}')"
start_server "$SRV_LOG" || exit 1
first_line="$(head -n1 "$SRV_LOG")"
log "server banner: $first_line"
if [ "$MODE" = "baseline" ]; then
    check "server reports baseline=1" bash -c "echo '$first_line' | grep -q 'baseline=1'"
else
    check "server reports baseline=0" bash -c "echo '$first_line' | grep -q 'baseline=0'"
fi

# ---------------------------------------------------------------------------
section "RB-DEMO-01: unknown command 'GET UNKNOWN' followed by 'PING' (same connection)"
out="$(printf 'GET UNKNOWN\nPING\n' | "$CLIENT_BIN" --host "$HOST" --port "$PORT" 2>"$EVIDENCE/robustness_${MODE}_rb01_client.err")"
rc=$?
log "client stdout:"; while IFS= read -r l; do log "  | $l"; done <<< "$out"
log "client stderr:"; while IFS= read -r l; do log "  | $l"; done < "$EVIDENCE/robustness_${MODE}_rb01_client.err"
log "client exit code: $rc"
line1="$(printf '%s\n' "$out" | sed -n 1p)"
line2="$(printf '%s\n' "$out" | sed -n 2p)"
if [ "$MODE" = "robust" ]; then
    expect_match "RB-01 unknown command answered with BAD_COMMAND" '^ERR code=BAD_COMMAND' "$line1"
    expect_match "RB-01 PING on the same connection still works" '^OK pong=1$' "$line2"
else
    observe "no BAD_COMMAND reply (stdout line 1 empty)" test -z "$line1"
    observe "connection closed by server (client logged EOF)" \
        grep -q 'server closed' "$EVIDENCE/robustness_${MODE}_rb01_client.err"
    observe "PING never answered" test -z "$line2"
fi
check "RB-01 parent server still alive" kill -0 "$SERVER_PID"
out="$(client_cmd PING)"
expect_match "RB-01 new connection after the incident works" '^OK pong=1$' "$out"

# ---------------------------------------------------------------------------
section "RB-DEMO-02: client connects and stays silent (--idle $IDLE_SEC); observe child each second"
"$CLIENT_BIN" --host "$HOST" --port "$PORT" --idle "$IDLE_SEC" \
    > "$EVIDENCE/robustness_${MODE}_rb02_client.out" 2> "$EVIDENCE/robustness_${MODE}_rb02_client.err" &
idle_pid=$!
sleep 0.5
# newest child of the server (-n): in baseline mode older zombies still exist
child_pid="$(pgrep -n -P "$SERVER_PID" || true)"
log "idle client pid=$idle_pid, server child pid=${child_pid:-none}"
alive_at=()
for t in $(seq 1 "$IDLE_SEC"); do
    sleep 1
    if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
        st="$(ps -o stat= -p "$child_pid" 2>/dev/null | tr -d ' ')"
        alive_at+=("$t")
        log "  t=${t}s child $child_pid alive stat=$st"
    else
        log "  t=${t}s child ${child_pid:-?} gone"
    fi
done
wait "$idle_pid" 2>/dev/null || true
log "idle client stdout: $(cat "$EVIDENCE/robustness_${MODE}_rb02_client.out")"
log "idle client stderr:"; while IFS= read -r l; do log "  | $l"; done < "$EVIDENCE/robustness_${MODE}_rb02_client.err"
if [ "${#alive_at[@]}" -gt 0 ]; then
    last_alive="${alive_at[$(( ${#alive_at[@]} - 1 ))]}"
else
    last_alive=0
fi
log "child was last seen alive at t=${last_alive}s (request deadline is 5 s)"
if [ "$MODE" = "robust" ]; then
    check "RB-02 child terminated by timeout within 7 s (last alive t=${last_alive}s)" test "$last_alive" -le 7
    check "RB-02 client received ERR code=TIMEOUT" grep -q 'ERR code=TIMEOUT' "$EVIDENCE/robustness_${MODE}_rb02_client.out"
    check "RB-02 server log mentions timeout" grep -q 'timeout' "$SRV_LOG"
else
    observe "child still alive after 12 s (no deadline)" test "$last_alive" -ge 12
    observe "client got no TIMEOUT reply" bash -c "! grep -q 'ERR code=TIMEOUT' '$EVIDENCE/robustness_${MODE}_rb02_client.out'"
fi
check "RB-02 parent server still alive" kill -0 "$SERVER_PID"
out="$(client_cmd PING)"
expect_match "RB-02 server still serves new clients" '^OK pong=1$' "$out"

# ---------------------------------------------------------------------------
section "RB-DEMO-03: 10 sequential clients, then look for zombies (stat Z)"
for i in $(seq 1 10); do
    out="$(client_cmd PING)"
    log "  client $i -> $out"
done
sleep 1
ps_out="$(ps_children "$SERVER_PID")"
log "ps -o pid,ppid,stat,etimes,cmd --ppid $SERVER_PID:"
while IFS= read -r l; do log "  | $l"; done <<< "$ps_out"
zombies="$(printf '%s\n' "$ps_out" | awk 'NR>1 && $3 ~ /^Z/ {c++} END{print c+0}')"
log "zombie children: $zombies"
if [ "$MODE" = "robust" ]; then
    check "RB-03 no zombie children (zombies=$zombies)" test "$zombies" -eq 0
else
    observe "zombies accumulated (zombies=$zombies, expected >= 10; earlier RB-01/RB-02 connections add a few more)" test "$zombies" -ge 10
fi
check "RB-03 parent server still alive" kill -0 "$SERVER_PID"

stop_server
log ""
log "--- server log ($MODE) ---"
while IFS= read -r l; do log "  $l"; done < "$SRV_LOG"

summary
