#!/usr/bin/env bash
# test_macos_port.sh: functional checks for the MESS 2.0 macOS port.
#
# Checks that the port RUNS correctly (builds, completes, produces non-zero
# latency/bandwidth, cleans up after itself, handles each CLI option). It does
# NOT judge whether the measured values are reasonable; that's analysis.
#
# Usage (from a built MESS tree, i.e. after `make` and `make install`):
#   bash test_macos_port.sh            # full battery, ~15-25 min
#   bash test_macos_port.sh quick      # dry runs + one short run, ~2 min
#
# Logs go to ./port_test_logs/. The Mac should be plugged in and otherwise idle.

set -u
MESS_ROOT="$(cd "$(dirname "$0")" && pwd)"
MESS="$MESS_ROOT/build/bin/mess"
LOGDIR="$MESS_ROOT/port_test_logs"
MODE="${1:-full}"
mkdir -p "$LOGDIR"

RESULTS=()
record() { RESULTS+=("$(printf '%-5s %-34s %s' "$1" "$2" "$3")"); echo "  -> $1: $3"; }

cleanup_procs() {
  pkill -f traffic_gen_multiseq 2>/dev/null
  pkill -f "/ptr_chase" 2>/dev/null
  sleep 1
}

# Run a command with a wall-clock limit (macOS has no `timeout`). Returns the
# command's exit code, or 124 if it was killed for exceeding the limit.
run_limited() {
  local limit="$1" log="$2"; shift 2
  "$@" > "$log" 2>&1 &
  local pid=$! waited=0 rc=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 2; waited=$((waited + 2))
    if [ "$waited" -ge "$limit" ]; then
      kill -INT "$pid" 2>/dev/null; sleep 5; kill -9 "$pid" 2>/dev/null
      cleanup_procs
      strip_ansi "$log"
      return 124
    fi
  done
  wait "$pid"; rc=$?
  strip_ansi "$log"
  return $rc
}

# MESS colours some output; remove terminal escape codes so grep matches plain text.
strip_ansi() {
  perl -pi -e 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' "$1" 2>/dev/null
}

# Common checks on a completed benchmark log.
check_run_log() {
  local name="$1" log="$2" rc="$3"
  if [ "$rc" -eq 124 ]; then record FAIL "$name" "timed out (see $log)"; return; fi
  if ! grep -q "Benchmark completed successfully" "$log"; then
    record FAIL "$name" "did not complete (rc=$rc; see $log)"; return
  fi
  local zero_bursts nostab lat_line
  zero_bursts=$(grep -c "dur: 0.000 s" "$log" || true)
  nostab=$(grep -c "did not stabilize" "$log" || true)
  lat_line=$(grep "Latency range:" "$log" | tail -1)
  if [ "$zero_bursts" -gt 0 ]; then
    record FAIL "$name" "$zero_bursts latency bursts with dur 0.000 s (pointer-chase fix missing?)"; return
  fi
  if echo "$lat_line" | grep -qE "Latency range: +0\.00 "; then
    record FAIL "$name" "a point reported 0.00 ns latency ($lat_line)"; return
  fi
  if grep -qE "Bandwidth range: +0\.00 " "$log"; then
    record FAIL "$name" "a point reported 0.00 GB/s bandwidth"; return
  fi
  if [ "$nostab" -gt 0 ]; then
    record WARN "$name" "completed; $nostab point(s) did not stabilize (used last sample)"
  else
    record PASS "$name" "completed; $(echo "$lat_line" | sed 's/^ *//')"
  fi
}

echo "MESS macOS port test: $MODE mode"
echo "Tree: $MESS_ROOT  (commit $(git -C "$MESS_ROOT" rev-parse --short HEAD 2>/dev/null || echo '?'))"
echo "macOS $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string), $(clang --version | head -1)"
[ -x "$MESS" ] || { echo "ERROR: $MESS not found. Run make && make install first."; exit 1; }
cleanup_procs; rm -rf /tmp/mess_sw_bw

# ---------------------------------------------------------------- 1. Build artefacts
echo "[1] Generated kernels"
if [ -x "$MESS_ROOT/build/bin/ptr_chase" ] && [ -x "$MESS_ROOT/build/bin/traffic_gen_multiseq.x" ]; then
  arr=$(grep -o "array: [0-9]* MB" "$MESS_ROOT/install.log" 2>/dev/null | tail -1)
  record PASS "kernels built" "ptr_chase + traffic_gen present (${arr:-array size unknown: no install.log})"
else
  record FAIL "kernels built" "missing ptr_chase or traffic_gen_multiseq.x (run make install)"
fi

# ---------------------------------------------------------------- 2. Dry runs
echo "[2] Dry run from repo root"
run_limited 60 "$LOGDIR/dry_root.log" "$MESS" --dry-run --verbose=2; rc=$?
if grep -qE "Initial status: *OK" "$LOGDIR/dry_root.log" && grep -qE "Sockets: *1" "$LOGDIR/dry_root.log"; then
  cores=$(grep -A2 "TrafficGen Configuration" "$LOGDIR/dry_root.log" | grep -oE "Cores: *[0-9]+")
  record PASS "dry run" "status OK, 1 socket, TrafficGen $cores"
else
  record FAIL "dry run" "rc=$rc; saw: $(grep -E "Initial status|Sockets" "$LOGDIR/dry_root.log" | tr -s ' ' | tr '\n' '|' | cut -c1-80)"
fi

echo "[3] Dry run from another directory (executable-path fix)"
( cd /tmp && run_limited 60 "$LOGDIR/dry_elsewhere.log" "$MESS" --dry-run ); rc=$?
if grep -qE "Initial status: *OK" "$LOGDIR/dry_elsewhere.log"; then
  record PASS "run outside repo dir" "OK from /tmp"
else
  record FAIL "run outside repo dir" "rc=$rc; saw: $(grep -E "Initial status|ERROR" "$LOGDIR/dry_elsewhere.log" | head -2 | tr -s ' ' | tr '\n' '|' | cut -c1-80)"
fi

# ---------------------------------------------------------------- 3. Short benchmark
echo "[4] Short run: all-read, two pauses"
run_limited 300 "$LOGDIR/short.log" "$MESS" --ratio=100 --pause=0,100 --repetitions=1 --verbose=3; rc=$?
check_run_log "short run (100% read)" "$LOGDIR/short.log" "$rc"
gens=$(grep -o "expected generators [0-9]*" "$LOGDIR/short.log" | head -1 | grep -o "[0-9]*$")
live=$(grep -o "live gens [0-9]*" "$LOGDIR/short.log" | tail -1 | grep -o "[0-9]*$")
if [ -n "$gens" ] && [ "$gens" = "$live" ]; then
  record PASS "generator count" "$live live counter files = $gens expected"
else
  record FAIL "generator count" "expected=${gens:-?} live=${live:-?} (see short.log [MacBW]/[Sample] lines)"
fi
cleanup_procs

if [ "$MODE" = "quick" ]; then
  echo; echo "=========== SUMMARY (quick) ==========="; printf '%s\n' "${RESULTS[@]}"; exit 0
fi

# ---------------------------------------------------------------- 4. Coverage runs
echo "[5] Read/write mixes (100%, 50%, 0% reads)"
run_limited 600 "$LOGDIR/mixes.log" "$MESS" --ratio=100,50,0 --pause=0,100 --repetitions=1 --verbose=3; rc=$?
check_run_log "ratios 100/50/0" "$LOGDIR/mixes.log" "$rc"
cleanup_procs

echo "[6] Adaptive pause discovery (--tier=lite)"
run_limited 900 "$LOGDIR/tier_lite.log" "$MESS" --ratio=100 --tier=lite --repetitions=1 --verbose=3; rc=$?
check_run_log "--tier=lite (adaptive pauses)" "$LOGDIR/tier_lite.log" "$rc"
cleanup_procs

echo "[7] Repetitions"
run_limited 600 "$LOGDIR/reps.log" "$MESS" --ratio=100 --pause=0,100 --repetitions=2 --verbose=3; rc=$?
check_run_log "--repetitions=2" "$LOGDIR/reps.log" "$rc"
cleanup_procs

echo "[8] Fewer traffic-generator cores"
run_limited 300 "$LOGDIR/cores4.log" "$MESS" --ratio=100 --pause=0 --repetitions=1 --total-cores=4 --verbose=3; rc=$?
check_run_log "--total-cores=4" "$LOGDIR/cores4.log" "$rc"
g4=$(grep -o "expected generators [0-9]*" "$LOGDIR/cores4.log" | head -1 | grep -o "[0-9]*$")
[ "$g4" = "4" ] && record PASS "--total-cores honored" "4 generators" \
                || record FAIL "--total-cores honored" "expected 4, got ${g4:-?}"
cleanup_procs

echo "[9] Persistent traffic generator"
run_limited 600 "$LOGDIR/persistent.log" "$MESS" --ratio=100 --pause=0,100 --repetitions=2 --persistent-trafficgen --verbose=3; rc=$?
check_run_log "--persistent-trafficgen" "$LOGDIR/persistent.log" "$rc"
cleanup_procs

echo "[10] Profile output files"
PROF_DIR="$LOGDIR/profile_out"; rm -rf "$PROF_DIR"
run_limited 300 "$LOGDIR/profile.log" "$MESS" --ratio=100 --pause=0,100 --repetitions=1 --profile --folder="$PROF_DIR" --verbose=1; rc=$?
nfiles=$(find "$PROF_DIR" -type f -size +0 2>/dev/null | wc -l | tr -d ' ')
if grep -q "Benchmark completed successfully" "$LOGDIR/profile.log" && [ "$nfiles" -gt 0 ]; then
  record PASS "--profile output" "$nfiles non-empty files under $PROF_DIR"
else
  record FAIL "--profile output" "rc=$rc, $nfiles files; see $LOGDIR/profile.log"
fi
cleanup_procs

# ---------------------------------------------------------------- 5. Robustness
echo "[11] Interrupt mid-run (Ctrl-C) and check cleanup"
"$MESS" --ratio=100 --pause=0,100 --repetitions=1 --verbose=1 > "$LOGDIR/interrupt.log" 2>&1 &
mpid=$!; sleep 20; kill -INT "$mpid" 2>/dev/null; sleep 8
kill -0 "$mpid" 2>/dev/null && { kill -9 "$mpid" 2>/dev/null; still="mess needed kill -9; "; } || still=""
left_tg=$(pgrep -f traffic_gen_multiseq | wc -l | tr -d ' ')
left_pc=$(pgrep -f "/ptr_chase" | wc -l | tr -d ' ')
if [ "$left_tg" -eq 0 ] && [ "$left_pc" -eq 0 ] && [ -z "$still" ]; then
  record PASS "Ctrl-C cleanup" "no leftover processes"
else
  record WARN "Ctrl-C cleanup" "${still}leftover: $left_tg traffic_gen, $left_pc ptr_chase (killed now)"
fi
cleanup_procs

echo "[12] Run again without clearing /tmp/mess_sw_bw (stale counter files)"
run_limited 300 "$LOGDIR/stale.log" "$MESS" --ratio=100 --pause=0 --repetitions=1 --verbose=3; rc=$?
check_run_log "stale counter files" "$LOGDIR/stale.log" "$rc"
cleanup_procs

# ---------------------------------------------------------------- 6. Options not expected to work on macOS
echo "[13] Linux-only options (recording behavior, not pass/fail)"
for opt in "--cores=0,1,2,3" "--bind=0" "--inst-lat" "--add-counters=cycles" "--measurer=perf"; do
  tag=$(echo "$opt" | tr -c 'a-z0-9' '_')
  run_limited 240 "$LOGDIR/opt_$tag.log" "$MESS" --ratio=100 --pause=0 --repetitions=1 "$opt" --verbose=1; rc=$?
  if grep -q "Benchmark completed successfully" "$LOGDIR/opt_$tag.log"; then
    record INFO "$opt" "ran to completion (check whether it actually had any effect)"
  elif [ "$rc" -eq 124 ]; then
    record INFO "$opt" "hung and was killed; document as unsupported"
  else
    msg=$(grep -iE "error|unsupported|not available|failed" "$LOGDIR/opt_$tag.log" | head -1 | cut -c1-70)
    record INFO "$opt" "exits with error: ${msg:-rc=$rc}"
  fi
  cleanup_procs
done

echo; echo "=========================== SUMMARY ==========================="
printf '%s\n' "${RESULTS[@]}"
echo "Logs: $LOGDIR"
