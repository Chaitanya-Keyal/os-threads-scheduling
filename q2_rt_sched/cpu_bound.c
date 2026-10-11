/*
 * CS F372 Assignment 2, Problem 2, Program A (CPU-bound)
 *
 * Forks 8 children that each run cpu_task() from workload_cpu.h and prints
 * context switch, response, turnaround and throughput statistics.
 *
 * Build: gcc -Wall -Wextra -O2 -o cpu_bound cpu_bound.c
 * Run:   sudo taskset -c 0 chrt -r 50 ./cpu_bound
 *        sudo timeout 30 taskset -c 0 chrt -f 50 ./cpu_bound
 */
#include "rt_bench.h"
#include "workload_cpu.h"

int main(void) { return run_bench(cpu_task); }
