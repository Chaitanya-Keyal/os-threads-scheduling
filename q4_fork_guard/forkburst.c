/*
 * CS F372 Assignment 2, Problem 4: a legitimate but bursty workload.
 *
 * Forks n short-lived children as fast as it can. When fork() fails with
 * EAGAIN (fork_guard's THROTTLE stage) it backs off for a little longer than
 * the fork_guard window and carries on. It is slowed down but never frozen
 * or killed, unlike a fork bomb, which keeps forking while throttled.
 *
 * Build: gcc -Wall -Wextra -O2 -o forkburst forkburst.c
 * Usage: ./forkburst [n] [backoff_ms]       (default 300 children, 1100 ms)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    int n = argc > 1 ? atoi(argv[1]) : 300;
    int backoff_ms = argc > 2 ? atoi(argv[2]) : 1100;
    int refused = 0;

    for (int i = 0; i < n;) {
        pid_t pid = fork();

        if (pid == 0) _exit(0);
        if (pid > 0) {
            i++;
        } else if (errno == EAGAIN) {
            refused++;
            printf("fork %d refused (EAGAIN), backing off %d ms\n", i + 1,
                   backoff_ms);
            usleep(backoff_ms * 1000);
        } else {
            perror("fork");
            return EXIT_FAILURE;
        }
        while (waitpid(-1, NULL, WNOHANG) > 0); /* reap finished children */
    }
    while (wait(NULL) > 0);
    printf("created %d children, %d forks refused\n", n, refused);
    return EXIT_SUCCESS;
}
