#!/usr/bin/env bash
# systemd entry point (see systemd/aqm.service).
# Starts the 5 stages and exits as soon as any one of them exits, so systemd
# restarts the whole pipeline. A frozen stage is caught by the watchdog
# instead: `report` pets it only while samples flow end to end.
set -u
BIN=${BIN:-./build/apps}

rm -f /dev/shm/aqm_shm /dev/mqueue/aqm_alarm     # leftovers from a crash

pids=()
names=(sense capture convert compare report)
"$BIN/aqm_sense" -p 80 -R & pids+=($!)
for _ in $(seq 50); do [ -e /dev/shm/aqm_shm ] && break; sleep 0.1; done
"$BIN/aqm_capture" -p 70 & pids+=($!)
"$BIN/aqm_convert" -p 60 & pids+=($!)
"$BIN/aqm_compare" -p 50 & pids+=($!)
"$BIN/aqm_report"  -p 0  & pids+=($!)
echo "aqm: started sense capture convert compare report (pids ${pids[*]})"

wait -n -p dead
rc=$?
stage=unknown
for i in "${!pids[@]}"; do [ "${pids[$i]}" = "${dead:-}" ] && stage=${names[$i]}; done
echo "aqm: stage $stage exited (rc $rc) - stopping the pipeline for a restart"
# for the OLED fault page after the restart: when, which stage, how
# (rc > 128 = killed by signal rc-128, e.g. 139 = segmentation fault)
mkdir -p data && echo "$(date +%s) $stage $rc" > data/last_exit
kill -TERM "${pids[@]}" 2>/dev/null
sleep 3
kill -KILL "${pids[@]}" 2>/dev/null
exit 1
