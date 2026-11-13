#!/usr/bin/env bash
# debug_matrix.sh - SPEC 6.1 / TC-07: compile-time x run-time debug matrix
#
#   DEBUG=0 build  without --debug  -> no [DEBUG]
#   DEBUG=0 build  with    --debug  -> no [DEBUG] (not compiled in)
#   DEBUG=1 build  without --debug  -> no [DEBUG]
#   DEBUG=1 build  with    --debug  -> [DEBUG] lines present
#
# Builds both modes (make / make DEBUG=1) into their own directories
# (build/release, build/debug) and runs PING for each combination.
# NOTE: the last `make` invoked here is DEBUG=1, so build/bin ends up
# pointing at the debug build; run `make` again to republish release.
#
# Usage: tests/debug_matrix.sh      Evidence: docs/evidence/debug_matrix.log

source "$(dirname "$0")/common.sh"

LOGFILE="$EVIDENCE/debug_matrix.log"
: > "$LOGFILE"

log "debug_matrix.sh start  port=$PORT"
section "Building release (DEBUG=0) and debug (DEBUG=1)"
make -s >/dev/null || { log "make failed"; exit 1; }
make -s DEBUG=1 >/dev/null || { log "make DEBUG=1 failed"; exit 1; }
log "built build/release and build/debug"

# Static evidence: a DEBUG-only format string must not exist in the
# release binaries at all (the message is compiled out, not just hidden).
section "Static check: DEBUG-only strings in binaries"
probe='accepted client fd='
rel_hits="$(strings build/release/bin/server | grep -c "$probe" || true)"
dbg_hits="$(strings build/debug/bin/server | grep -c "$probe" || true)"
log "strings | grep '$probe' : release=$rel_hits debug=$dbg_hits"
check "release server does not contain the DEBUG string" test "$rel_hits" -eq 0
check "debug server contains the DEBUG string" test "$dbg_hits" -ge 1

run_case() {
    local build="$1" flag="$2" expect="$3"
    local tag="${build}_${flag:-noflag}"
    local srv_log="$EVIDENCE/debug_matrix_${tag}_server.log"
    local cli_log="$EVIDENCE/debug_matrix_${tag}_client.log"

    BIN_DIR="$ROOT_DIR/build/$build/bin"
    SERVER_BIN="$BIN_DIR/server"
    CLIENT_BIN="$BIN_DIR/client"

    section "Case: build=$build runtime=${flag:-(none)} -> expect DEBUG shown: $expect"
    # shellcheck disable=SC2086
    start_server "$srv_log" $flag || return 1
    # shellcheck disable=SC2086
    out="$("$CLIENT_BIN" --host "$HOST" --port "$PORT" --command PING $flag 2> "$cli_log")"
    log "client stdout: $out"
    stop_server
    sleep 0.2

    local s_dbg c_dbg
    s_dbg="$(grep -c '\[DEBUG\]' "$srv_log" || true)"
    c_dbg="$(grep -c '\[DEBUG\]' "$cli_log" || true)"
    log "server [DEBUG] lines: $s_dbg   client [DEBUG] lines: $c_dbg"
    log "server log:"; while IFS= read -r l; do log "  | $l"; done < "$srv_log"
    log "client log:"; while IFS= read -r l; do log "  | $l"; done < "$cli_log"

    expect_match "case $tag: PING works" '^OK pong=1$' "$out"
    if [ "$expect" = "yes" ]; then
        check "case $tag: server shows [DEBUG] ($s_dbg)" test "$s_dbg" -gt 0
        check "case $tag: client shows [DEBUG] ($c_dbg)" test "$c_dbg" -gt 0
    else
        check "case $tag: server shows no [DEBUG] ($s_dbg)" test "$s_dbg" -eq 0
        check "case $tag: client shows no [DEBUG] ($c_dbg)" test "$c_dbg" -eq 0
    fi
    check "case $tag: INFO lines still present" grep -q '\[INFO\]' "$srv_log"
}

run_case release ""        no
run_case release "--debug" no
run_case debug   ""        no
run_case debug   "--debug" yes

section "Matrix"
log "| Compile-time | Runtime  | DEBUG shown |"
log "| DEBUG=0      | (none)   | no          |"
log "| DEBUG=0      | --debug  | no          |"
log "| DEBUG=1      | (none)   | no          |"
log "| DEBUG=1      | --debug  | yes         |"

summary
