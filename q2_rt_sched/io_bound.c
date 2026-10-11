/*
 * CS F372 Assignment 2, Problem 2, Program B (I/O-bound)
 *
 * Forks 8 children that each run io_task() from workload_io.h and prints
 * context switch, response, turnaround and throughput statistics.
 *
 * Build: gcc -Wall -Wextra -O2 -o io_bound io_bound.c
 * Run:   sudo taskset -c 0 chrt -r 50 ./io_bound
 *        sudo timeout 30 taskset -c 0 chrt -f 50 ./io_bound
 */
#include "rt_bench.h"
#include "workload_io.h"

int main(void) { return run_bench(io_task); }
