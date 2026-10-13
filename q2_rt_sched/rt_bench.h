/*
 * CS F372 Assignment 2, Problem 2: measurement harness shared by
 * Program A (cpu_bound.c) and Program B (io_bound.c).
 *
 * The parent records a reference timestamp, creates a pipe and forks
 * NUM_CHILDREN children. Each child records its start time as its first
 * action, runs the workload, records its completion time and sends both
 * through the pipe. The parent then computes response and turnaround times
 * against its reference timestamp, and reads the children's context switch
 * counts with getrusage(RUSAGE_CHILDREN) once all of them are reaped.
 *
 * If the run is cut short by `timeout` (SIGTERM), the parent stops waiting
 * and reports the children that had finished by then.
 *
 * The scheduling policy is not set here: the program is started under
 * `chrt -r` or `chrt -f` and the children inherit the policy on fork().
 */
#ifndef RT_BENCH_H
#define RT_BENCH_H

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NUM_CHILDREN 8

struct child_report {
    struct timespec start; /* first action after fork() returns */
    struct timespec end;   /* just before the child exits */
};

static double ms_between(struct timespec from, struct timespec to) {
    return (to.tv_sec - from.tv_sec) * 1e3 + (to.tv_nsec - from.tv_nsec) / 1e6;
}

/* "Time Quantum: Nms" under SCHED_RR (the kernel's actual quantum, read
 * back with sched_rr_get_interval), the policy name otherwise. */
static void print_policy(void) {
    int policy = sched_getscheduler(0);
    struct timespec q;

    if (policy == SCHED_RR && sched_rr_get_interval(0, &q) == 0)
        printf("Time Quantum: %.0fms\n", q.tv_sec * 1e3 + q.tv_nsec / 1e6);
    else
        printf("Scheduling Policy: %s\n",
               policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER");
}

/* Only has to exist: a handled signal makes the parent's read() return. */
static void on_sigterm(int sig) { (void)sig; }

static int run_bench(void (*task)(int)) {
    struct sigaction sa = {.sa_handler = on_sigterm}; /* no SA_RESTART */
    struct timespec ref, done;
    struct child_report rep;
    struct rusage usage;
    double resp, turn, sum_resp = 0, max_resp = 0, sum_turn = 0, max_turn = 0;
    int fd[2], got = 0;

    if (pipe(fd) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    clock_gettime(CLOCK_MONOTONIC, &ref);

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (pid == 0) {
            clock_gettime(CLOCK_MONOTONIC, &rep.start);
            close(fd[0]);
            task(i);
            clock_gettime(CLOCK_MONOTONIC, &rep.end);
            /* a write of less than PIPE_BUF bytes is atomic */
            if (write(fd[1], &rep, sizeof(rep)) != sizeof(rep)) _exit(1);
            _exit(0);
        }
    }
    close(fd[1]); /* read() returns 0 once every child has exited */

    /* `timeout 30` stops a run with SIGTERM. The children were forked before
     * this, so they keep the default action and are terminated. */
    sigaction(SIGTERM, &sa, NULL);

    while (read(fd[0], &rep, sizeof(rep)) == sizeof(rep)) {
        resp = ms_between(ref, rep.start);
        turn = ms_between(ref, rep.end);
        sum_resp += resp;
        sum_turn += turn;
        if (resp > max_resp) max_resp = resp;
        if (turn > max_turn) max_turn = turn;
        got++;
    }
    close(fd[0]);
    while (wait(NULL) > 0);
    clock_gettime(CLOCK_MONOTONIC, &done);
    getrusage(RUSAGE_CHILDREN, &usage); /* totals of all reaped children */

    double elapsed = ms_between(ref, done) / 1e3;

    print_policy();
    printf("  Involuntary Context Switches: %ld\n", usage.ru_nivcsw);
    printf("  Voluntary Context Switches:   %ld\n", usage.ru_nvcsw);
    if (got > 0) {
        printf("  Average Response Time:        %.2fms\n", sum_resp / got);
        printf("  Maximum Response Time:        %.2fms\n", max_resp);
        printf("  Average Turnaround Time:      %.2fms\n", sum_turn / got);
        printf("  Maximum Turnaround Time:      %.2fms\n", max_turn);
    }
    printf("  Throughput:                   %.2f children/s\n", got / elapsed);
    printf("  Total Elapsed Time:           %.2fs\n", elapsed);
    if (got != NUM_CHILDREN)
        printf("  (stopped early: %d of %d children finished)\n", got,
               NUM_CHILDREN);
    return EXIT_SUCCESS;
}

#endif
