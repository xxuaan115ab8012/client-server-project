#!/usr/bin/env bash
# concurrent_10.sh - SPEC 11.2: 10 clients served simultaneously
#
# Starts the server with --demo --debug, launches N (default 10) clients
# in parallel each sending "DEMO WAIT 3", captures `ps` of the server's
# children while they are all waiting, measures total wall time and
# counts the child PIDs reported in the server log.
#
# Usage: tests/concurrent_10.sh            (N=10, port 9090, build/bin)
#        N=12 PORT=9191 tests/concurrent_10.sh
# Evidence: docs/evidence/concurrent_10.log
#           docs/evidence/concurrent_10_server.log
#           docs/evidence/concurrent_10_ps.txt

source "$(dirname "$0")/common.sh"
require_binaries

N="${N:-10}"
WAIT_SEC="${WAIT_SEC:-3}"
LOGFILE="$EVIDENCE/concurrent_10.log"
: > "$LOGFILE"
SRV_LOG="$EVIDENCE/concurrent_10_server.log"
PS_FILE="$EVIDENCE/concurrent_10_ps.txt"
OUT_DIR="$(mktemp -d /tmp/lsm_conc.XXXXXX)"
trap 'rm -rf "$OUT_DIR"; cleanup_common' EXIT

log "concurrent_10.sh start  N=$N wait=${WAIT_SEC}s bin=$BIN_DIR port=$PORT"
start_server "$SRV_LOG" --demo --debug || exit 1

section "Launching $N clients with 'DEMO WAIT $WAIT_SEC' in parallel"
t_start=$(date +%s.%N)
pids=()
for i in $(seq 1 "$N"); do
    ( "$CLIENT_BIN" --host "$HOST" --port "$PORT" --command "DEMO WAIT $WAIT_SEC" \
        > "$OUT_DIR/client_$i.out" 2> "$OUT_DIR/client_$i.err"
      echo $? > "$OUT_DIR/client_$i.rc" ) &
    pids+=($!)
done
log "launched $N clients at $(ts)"

# Evidence 2: snapshot of the server's children while they are waiting.
sleep 1
{
    echo "# $(ts)  ps -o pid,ppid,stat,etimes,cmd --ppid $SERVER_PID  (taken ~1 s after launch)"
    ps_children "$SERVER_PID"
} > "$PS_FILE"
alive_children="$(pgrep -P "$SERVER_PID" | wc -l)"
log "children alive 1 s after launch: $alive_children"
while IFS= read -r l; do log "  $l"; done < "$PS_FILE"

# Wait for all clients
for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
t_end=$(date +%s.%N)
elapsed="$(awk -v a="$t_start" -v b="$t_end" 'BEGIN{printf "%.2f", b - a}')"

section "Results"
ok_count=0
for i in $(seq 1 "$N"); do
    resp="$(cat "$OUT_DIR/client_$i.out" 2>/dev/null)"
    rc="$(cat "$OUT_DIR/client_$i.rc" 2>/dev/null)"
    log "client $i: rc=$rc resp='$resp'"
    if [ "$resp" = "OK waited_sec=$WAIT_SEC" ] && [ "$rc" = "0" ]; then
        ok_count=$((ok_count + 1))
    fi
done

sleep 0.5
child_pids="$(grep -o 'forked child pid=[0-9]*' "$SRV_LOG" | awk -F= '{print $2}' | sort -u)"
child_pid_count="$(printf '%s\n' "$child_pids" | grep -c .)"
started_count="$(grep -c 'child started' "$SRV_LOG")"
exited_count="$(grep -c 'child exited' "$SRV_LOG")"
remaining="$(pgrep -P "$SERVER_PID" | wc -l)"

log "total wall time for $N parallel 'DEMO WAIT $WAIT_SEC': ${elapsed}s (sequential would be ~$((N * WAIT_SEC))s)"
log "distinct child PIDs in server log: $child_pid_count -> $(echo $child_pids | tr '\n' ' ')"
log "server log: 'child started'=$started_count 'child exited'=$exited_count"
log "children still alive after test: $remaining"

check "all $N clients received 'OK waited_sec=$WAIT_SEC' ($ok_count/$N)" test "$ok_count" -eq "$N"
check "at least $N children alive simultaneously ($alive_children)" test "$alive_children" -ge "$N"
check "server log shows $N distinct child PIDs ($child_pid_count)" test "$child_pid_count" -ge "$N"
check "wall time < $((WAIT_SEC * 3))s (concurrent, not sequential): ${elapsed}s" \
    awk -v e="$elapsed" -v lim="$((WAIT_SEC * 3))" 'BEGIN{exit !(e < lim)}'
check "no children left after completion ($remaining)" test "$remaining" -eq 0

stop_server
log ""
log "--- server log ---"
while IFS= read -r l; do log "  $l"; done < "$SRV_LOG"

summary
