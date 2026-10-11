#!/bin/bash
# CS F372 Assignment 2, Problem 2: runs both programs under SCHED_RR for each
# time quantum and once under SCHED_FIFO, all pinned to CPU 0, and writes
# the statistics to results.txt.
#
# Timings inside the VM vary by about 20% from run to run, so every
# configuration is run RUNS times and the run with the median total elapsed
# time is reported.
#
# Usage: sudo ./run_all.sh        (after `make` in the repository root)

QUANTA="1 10 50 150 300 500"
PRIO=50
RUNS=3
OUT=results.txt

cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

# Put the quantum back to what it was when the script exits.
old=$(sysctl -n kernel.sched_rr_timeslice_ms)
tmp=$(mktemp -d)
trap 'sysctl -qw kernel.sched_rr_timeslice_ms=$old; rm -rf "$tmp"' EXIT

median_run() { # median_run <command...>
    for i in $(seq $RUNS); do
        "$@" >"$tmp/$i" || echo "  (stopped by timeout 30)" >>"$tmp/$i"
        echo "$(grep -oP 'Total Elapsed Time:\s+\K[\d.]+' "$tmp/$i") $i"
    done | sort -n | sed -n "$(((RUNS + 1) / 2))p" | {
        read -r _ i
        cat "$tmp/$i"
    }
}

run() { # run <program> <title>
    echo "$2"
    echo "-------------------------------------------------"
    for q in $QUANTA; do
        sysctl -qw kernel.sched_rr_timeslice_ms="$q"
        median_run taskset -c 0 chrt -r $PRIO "./$1"
        echo
    done
    median_run timeout 30 taskset -c 0 chrt -f $PRIO "./$1"
    echo
}

{
    echo "CS F372 Assignment 2 - Problem 2 results"
    echo "Kernel $(uname -r), HZ=$(grep -oP '^CONFIG_HZ=\K\d+' "/boot/config-$(uname -r)"), $(nproc) CPUs, all processes pinned to CPU 0, RT priority $PRIO"
    echo "Each configuration was run $RUNS times; the run with the median total elapsed time is shown."
    echo
    run cpu_bound "Program-A (CPU-Bound)"
    run io_bound "Program-B (I/O-Bound)"
} | tee $OUT
