#!/usr/bin/env bash
# test_protocol.sh - protocol / functional tests (SPEC TC-01..TC-06 + errors)
#
# Usage: tests/test_protocol.sh            (uses build/bin, port 9090)
#        PORT=9191 tests/test_protocol.sh
# Evidence: docs/evidence/test_protocol.log

source "$(dirname "$0")/common.sh"
require_binaries

LOGFILE="$EVIDENCE/test_protocol.log"
: > "$LOGFILE"
SRV_LOG="$EVIDENCE/test_protocol_server.log"

log "test_protocol.sh start  bin=$BIN_DIR port=$PORT"
start_server "$SRV_LOG" || exit 1

# --- TC-01 PING ------------------------------------------------------------
section "TC-01 PING"
out="$(client_cmd PING)"; rc=$?
expect_match "PING response" '^OK pong=1$' "$out"
check "PING exit code 0" test "$rc" -eq 0

# --- TC-02 GET ALL ---------------------------------------------------------
section "TC-02 GET ALL"
out="$(client_cmd "GET ALL")"
log "GET ALL -> $out"
expect_match "GET ALL starts with OK" '^OK ' "$out"
for key in hostname os kernel cpu_cores cpu_usage_pct mem_total_kb mem_available_kb uptime_sec; do
    expect_match "GET ALL has $key" "( |^)$key=[^ ]+" "$out"
done

# --- TC-03 individual queries ---------------------------------------------
section "TC-03 individual queries"
out="$(client_cmd "GET CPU")"
expect_match "GET CPU" '^OK cpu_cores=[0-9]+ cpu_usage_pct=[0-9]+\.[0-9]$' "$out"
out="$(client_cmd "GET MEMORY")"
expect_match "GET MEMORY" '^OK mem_total_kb=[0-9]+ mem_available_kb=[0-9]+$' "$out"
out="$(client_cmd "GET UPTIME")"
expect_match "GET UPTIME" '^OK uptime_sec=[0-9]+$' "$out"
out="$(client_cmd "GET HOSTNAME")"
expect_match "GET HOSTNAME" '^OK hostname=[^ ]+$' "$out"
# sanity: hostname equals the local hostname (spaces normalised)
expected_host="$(hostname | tr ' ' '_')"
expect_match "hostname matches \`hostname\`" "^OK hostname=${expected_host}$" "$out"

# --- TC-04 three commands on one connection (interactive mode via pipe) ---
section "TC-04 three commands on one persistent connection"
out="$(printf 'PING\nGET UPTIME\nGET HOSTNAME\nQUIT\n' | "$CLIENT_BIN" --host "$HOST" --port "$PORT" 2>/dev/null)"
log "$out"
lines=$(printf '%s\n' "$out" | grep -c .)
check "4 responses received" test "$lines" -eq 4
expect_match "line1 pong"     '^OK pong=1$'          "$(printf '%s\n' "$out" | sed -n 1p)"
expect_match "line2 uptime"   '^OK uptime_sec=[0-9]+$' "$(printf '%s\n' "$out" | sed -n 2p)"
expect_match "line3 hostname" '^OK hostname=[^ ]+$'  "$(printf '%s\n' "$out" | sed -n 3p)"
expect_match "line4 bye"      '^OK bye=1$'           "$(printf '%s\n' "$out" | sed -n 4p)"

# CRLF tolerance
out="$(printf 'PING\r\n' | "$CLIENT_BIN" --host "$HOST" --port "$PORT" 2>/dev/null | head -n1)"
expect_match "CRLF request accepted" '^OK pong=1$' "$out"

# --- TC-05 QUIT closes the child --------------------------------------------
section "TC-05 QUIT terminates child"
out="$(client_cmd QUIT)"
expect_match "QUIT response" '^OK bye=1$' "$out"
sleep 0.5
kids="$(pgrep -P "$SERVER_PID" | wc -l)"
check "no child remains after QUIT (children=$kids)" test "$kids" -eq 0
check "server log shows 'child exited'" grep -q 'child exited' "$SRV_LOG"

# --- error handling ----------------------------------------------------------
section "Error responses"
out="$(client_cmd "GET UNKNOWN")"; rc=$?
expect_match "GET UNKNOWN -> BAD_COMMAND" '^ERR code=BAD_COMMAND detail=[^ ]+$' "$out"
check "client exits non-zero on ERR (rc=$rc)" test "$rc" -ne 0

out="$(client_cmd "FOO")"
expect_match "FOO -> BAD_COMMAND" '^ERR code=BAD_COMMAND' "$out"

out="$(client_cmd "GET")"
expect_match "GET (no target) -> INVALID_ARGUMENT" '^ERR code=INVALID_ARGUMENT' "$out"

out="$(client_cmd "DEMO WAIT 2")"
expect_match "DEMO without --demo -> DEMO_DISABLED" '^ERR code=DEMO_DISABLED' "$out"

# BAD_COMMAND keeps the connection: GET UNKNOWN then PING on same connection
out="$(printf 'GET UNKNOWN\nPING\n' | "$CLIENT_BIN" --host "$HOST" --port "$PORT" 2>/dev/null)"
expect_match "after BAD_COMMAND connection still works" 'OK pong=1' "$(printf '%s\n' "$out" | sed -n 2p)"

# REQUEST_TOO_LONG: 300 'A's (line incl. LF = 301 bytes > 256)
long="$(head -c 300 /dev/zero | tr '\0' 'A')"
out="$(client_cmd "$long")"
expect_match "300-byte request -> REQUEST_TOO_LONG" '^ERR code=REQUEST_TOO_LONG' "$out"
# exactly 255 chars + LF = 256 bytes must be accepted (as BAD_COMMAND)
edge="$(head -c 255 /dev/zero | tr '\0' 'B')"
out="$(client_cmd "$edge")"
expect_match "255-byte request accepted (BAD_COMMAND)" '^ERR code=BAD_COMMAND' "$out"
# 256 chars + LF = 257 bytes must be rejected
edge="$(head -c 256 /dev/zero | tr '\0' 'C')"
out="$(client_cmd "$edge")"
expect_match "256-byte request -> REQUEST_TOO_LONG" '^ERR code=REQUEST_TOO_LONG' "$out"

# --- DEMO mode -----------------------------------------------------------------
section "DEMO mode (--demo)"
stop_server
start_server "$EVIDENCE/test_protocol_server_demo.log" --demo || exit 1
t0=$(date +%s.%N)
out="$(client_cmd "DEMO WAIT 1")"
t1=$(date +%s.%N)
expect_match "DEMO WAIT 1" '^OK waited_sec=1$' "$out"
elapsed="$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b - a}')"
log "DEMO WAIT 1 took ${elapsed}s"
out="$(client_cmd "DEMO WAIT 9")"
expect_match "DEMO WAIT 9 -> INVALID_ARGUMENT" '^ERR code=INVALID_ARGUMENT' "$out"
out="$(client_cmd "DEMO WAIT x")"
expect_match "DEMO WAIT x -> INVALID_ARGUMENT" '^ERR code=INVALID_ARGUMENT' "$out"
out="$(client_cmd "DEMO FOO")"
expect_match "DEMO FOO -> BAD_COMMAND" '^ERR code=BAD_COMMAND' "$out"
stop_server

# --- TC-06 ldd -----------------------------------------------------------------
section "TC-06 ldd shows libcommon.so"
ldd_out="$(ldd "$SERVER_BIN"; echo '---'; ldd "$CLIENT_BIN")"
log "$ldd_out"
check "server links libcommon.so" bash -c "ldd '$SERVER_BIN' | grep -q 'libcommon.so'"
check "client links libcommon.so" bash -c "ldd '$CLIENT_BIN' | grep -q 'libcommon.so'"
check "libcommon.so resolved (not 'not found')" bash -c "! ldd '$SERVER_BIN' | grep -q 'not found'"
{ echo "# ldd $SERVER_BIN"; ldd "$SERVER_BIN"; echo; echo "# ldd $CLIENT_BIN"; ldd "$CLIENT_BIN"; } > "$EVIDENCE/ldd.txt"

# --- client CLI validation ---------------------------------------------------
section "Client CLI validation"
"$CLIENT_BIN" --idle 1 --command PING >/dev/null 2>&1; rc=$?
check "--idle with --command rejected (rc=$rc)" test "$rc" -eq 2
"$CLIENT_BIN" --port 99999 >/dev/null 2>&1; rc=$?
check "--port 99999 rejected (rc=$rc)" test "$rc" -eq 2

log ""
log "--- server log (first run) ---"
while IFS= read -r l; do log "  $l"; done < "$SRV_LOG"

summary
