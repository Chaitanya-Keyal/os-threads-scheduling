# CS F372 - Assignment 2
# Builds every user-space program. Problem 4's kernel part is documented in
# q4_fork_guard/README.md (it needs a kernel rebuild and cannot be built here).

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread

PROGRAMS = q1_report_card/report_card \
           q2_rt_sched/cpu_bound \
           q2_rt_sched/io_bound \
           q3_sudoku/sudoku_analyzer \
           q4_fork_guard/tier \
           q4_fork_guard/forkburst

all: $(PROGRAMS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

q2_rt_sched/cpu_bound: q2_rt_sched/rt_bench.h q2_rt_sched/workload_cpu.h
q2_rt_sched/io_bound: q2_rt_sched/rt_bench.h q2_rt_sched/workload_io.h

clean:
	rm -f $(PROGRAMS)

# Bundle sources, executables, readme, results and patch for submission.
dist: all
	tar czf assignment2_submission.tar.gz --exclude=.clang-format README.md Makefile \
	    q1_report_card q2_rt_sched q3_sudoku q4_fork_guard

.PHONY: all clean dist
