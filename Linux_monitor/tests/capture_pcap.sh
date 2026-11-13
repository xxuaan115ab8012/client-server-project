#!/usr/bin/env bash
# capture_pcap.sh - SPEC 11.3 / TC-08: capture loopback traffic with tcpdump
#
# Runs tcpdump on the loopback interface while a client performs
# "GET ALL" (one connection: handshake, request, response, QUIT, FIN) and
# a second client performs PING. Writes:
#   captures/loopback.pcap
#   docs/evidence/pcap_summary.txt   (tcpdump -nn -r, one line per packet)
#   docs/evidence/pcap_ascii.txt     (tcpdump -nn -A -r, with payload)
#
# tcpdump needs root.  Either run the whole script with sudo, or set
# SUDO_PASS in the environment (it is piped to `sudo -S`, never stored).
#
# Usage: tests/capture_pcap.sh          or   sudo -E tests/capture_pcap.sh

source "$(dirname "$0")/common.sh"
require_binaries

LOGFILE="$EVIDENCE/capture_pcap.log"
: > "$LOGFILE"
PCAP="$ROOT_DIR/captures/loopback.pcap"
mkdir -p "$ROOT_DIR/captures"

SUDO=()
if [ "$(id -u)" -ne 0 ]; then
    if [ -n "${SUDO_PASS:-}" ]; then
        SUDO=(sudo -S -p "")
    else
        SUDO=(sudo)
    fi
fi

run_sudo() {
    if [ "${#SUDO[@]}" -eq 0 ]; then
        "$@"
    elif [ -n "${SUDO_PASS:-}" ]; then
        printf '%s\n' "$SUDO_PASS" | "${SUDO[@]}" "$@"
    else
        "${SUDO[@]}" "$@"
    fi
}

if ! command -v tcpdump >/dev/null 2>&1; then
    log "ERROR: tcpdump not installed (sudo apt-get install -y tcpdump)"
    exit 1
fi

log "capture_pcap.sh start  port=$PORT pcap=$PCAP"
start_server "$EVIDENCE/capture_pcap_server.log" || exit 1

section "Starting tcpdump on lo (tcp port $PORT)"
rm -f "$PCAP"
# -Z root keeps the file writable in captures/ (tcpdump otherwise drops
# privileges before opening the output file); -s 0 full packets.
run_sudo tcpdump -i lo -nn -s 0 -Z root -w "$PCAP" "tcp port $PORT" \
    > "$EVIDENCE/capture_pcap_tcpdump.log" 2>&1 &
TCPDUMP_SH_PID=$!
sleep 2
# the real tcpdump process (not the sudo wrapper): cmdline starts with tcpdump
TCPDUMP_PID="$(pgrep -f "^tcpdump -i lo -nn -s 0 -Z root -w $PCAP" | head -n1 || true)"
log "tcpdump started (wrapper pid=$TCPDUMP_SH_PID, tcpdump pid=${TCPDUMP_PID:-?})"
if [ -z "$TCPDUMP_PID" ]; then
    log "ERROR: tcpdump did not start (sudo password? see $EVIDENCE/capture_pcap_tcpdump.log)"
    cat "$EVIDENCE/capture_pcap_tcpdump.log"
    exit 1
fi

section "Generating traffic"
out="$(client_cmd "GET ALL")"
log "client 1: GET ALL -> $out"
expect_match "GET ALL succeeded" '^OK hostname=' "$out"
out="$(client_cmd "PING")"
log "client 2: PING -> $out"
expect_match "PING succeeded" '^OK pong=1$' "$out"
sleep 1

section "Stopping tcpdump"
# SIGTERM (not SIGINT): background jobs of a non-interactive shell inherit
# SIGINT=ignored, so tcpdump would never notice an INT.  tcpdump flushes
# and closes the file on TERM as well.
run_sudo kill -TERM "$TCPDUMP_PID" 2>/dev/null || true
for _ in $(seq 1 50); do
    kill -0 "$TCPDUMP_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$TCPDUMP_PID" 2>/dev/null; then
    log "tcpdump still running, forcing"
    run_sudo kill -KILL "$TCPDUMP_PID" 2>/dev/null || true
fi
wait "$TCPDUMP_SH_PID" 2>/dev/null || true
sleep 0.5
while IFS= read -r l; do log "  | $l"; done < "$EVIDENCE/capture_pcap_tcpdump.log"
stop_server

check "pcap file created" test -s "$PCAP"
if [ -s "$PCAP" ]; then
    run_sudo chmod a+r "$PCAP" 2>/dev/null || true
    tcpdump -nn -r "$PCAP" > "$EVIDENCE/pcap_summary.txt" 2>&1 || true
    tcpdump -nn -A -r "$PCAP" > "$EVIDENCE/pcap_ascii.txt" 2>&1 || true
    pkts="$(grep -c "IP 127.0.0.1" "$EVIDENCE/pcap_summary.txt" || true)"
    log "packets captured: $pkts"
    log "--- tcpdump -nn -r $PCAP ---"
    while IFS= read -r l; do log "  $l"; done < "$EVIDENCE/pcap_summary.txt"
    check "pcap contains SYN (handshake)"       grep -q 'Flags \[S\]' "$EVIDENCE/pcap_summary.txt"
    check "pcap contains SYN-ACK"               grep -q 'Flags \[S\.\]' "$EVIDENCE/pcap_summary.txt"
    check "pcap contains FIN"                   grep -q 'Flags \[F\.\]' "$EVIDENCE/pcap_summary.txt"
    check "pcap payload shows 'GET ALL'"        grep -q 'GET ALL' "$EVIDENCE/pcap_ascii.txt"
    check "pcap payload shows 'OK hostname='"   grep -q 'OK hostname=' "$EVIDENCE/pcap_ascii.txt"
    check "pcap payload shows 'OK bye=1'"       grep -q 'OK bye=1' "$EVIDENCE/pcap_ascii.txt"
fi

summary
