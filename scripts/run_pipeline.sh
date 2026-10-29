#!/usr/bin/env bash
# Start all 5 stages with their real-time priorities; Ctrl+C stops everything.
# Run from the repo root:  ./scripts/run_pipeline.sh
set -uo pipefail

BIN=${BIN:-./build/apps}
LOG=${LOG:-./logs}
mkdir -p "$LOG"

pids=()

start() {
    local name=$1; shift
    "$BIN/aqm_$name" "$@" > "$LOG/$name.log" 2>&1 &
    pids+=($!)
    echo "started $name (pid $!) $*"
}

# true while any stage is still running (zombies count as exited)
any_alive() {
    local p
    for p in "${pids[@]}"; do
        if [ -d "/proc/$p" ] && ! grep -q '^State:.*Z' "/proc/$p/status" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

stop() {
    trap - INT TERM
    echo
    echo "stopping all stages..."
    kill -TERM "${pids[@]}" 2>/dev/null

    # give stages up to 3 s to exit cleanly, then force
    for _ in $(seq 30); do
        any_alive || break
        sleep 0.1
    done
    if any_alive; then
        echo "some stages did not exit, sending SIGKILL"
        kill -KILL "${pids[@]}" 2>/dev/null
    fi
    wait "${pids[@]}" 2>/dev/null

    rm -f /dev/shm/aqm_shm
    for n in sense capture convert compare report; do
        tail -n 1 "$LOG/$n.log"
    done
    echo "all stopped. Logs in $LOG/"
    exit 0
}
trap stop INT TERM

# priority ladder: the closer to the sensor, the higher the priority
start sense   -p 80
for _ in $(seq 50); do [ -e /dev/shm/aqm_shm ] && break; sleep 0.1; done
start capture -p 70
start convert -p 60
start compare -p 50
start report  -p 0

echo "running. Ctrl+C to stop. Showing report output:"
echo

# foreground tail: Ctrl+C ends it, then the trap stops the stages
tail -n +1 -f "$LOG/report.log"
stop
