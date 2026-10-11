/*
 * CS F372 Assignment 2, Problem 4: front end for the sched_tier and
 * fork_guard kernel interfaces.
 *
 *   tier set <username> <gold|silver|bronze>   (root only)
 *   tier reset                                 (root only)
 *   tier show
 *
 * Build: gcc -Wall -Wextra -O2 -o tier tier.c
 * Install: sudo install tier /usr/local/bin/
 */
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TIER_PROC "/proc/sched_tier"
#define GUARD_PROC "/proc/fork_guard"

static int send_command(const char* cmd) {
    int fd = open(TIER_PROC, O_WRONLY);

    if (fd < 0 || write(fd, cmd, strlen(cmd)) < 0) {
        perror("tier: " TIER_PROC);
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* Reads a whole /proc file into buf as a string; returns 0 or -1. */
static int read_proc(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "r");
    size_t len;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    len = fread(buf, 1, size - 1, f);
    buf[len] = '\0';
    fclose(f);
    return 0;
}

static int show(void) {
    char buf[8192];

    if (read_proc(TIER_PROC, buf, sizeof(buf)) < 0) return EXIT_FAILURE;
    if (strchr(buf, '\n') == strrchr(buf, '\n')) /* header line only */
        printf("(no user is tiered; everyone is SILVER)\n");
    else
        fputs(buf, stdout);
    if (read_proc(GUARD_PROC, buf, sizeof(buf)) < 0) return EXIT_FAILURE;
    printf("\n%s", buf);
    return EXIT_SUCCESS;
}

static int set(const char* user, const char* tier) {
    struct passwd* pw = getpwnam(user);
    char cmd[64];

    if (pw == NULL) {
        fprintf(stderr, "tier: no such user '%s'\n", user);
        return EXIT_FAILURE;
    }
    if (strcasecmp(tier, "gold") && strcasecmp(tier, "silver") &&
        strcasecmp(tier, "bronze")) {
        fprintf(stderr, "tier: tier must be gold, silver or bronze\n");
        return EXIT_FAILURE;
    }
    snprintf(cmd, sizeof(cmd), "set %u %s", (unsigned)pw->pw_uid, tier);
    if (send_command(cmd) < 0) return EXIT_FAILURE;
    printf("user %s (uid %u) -> %s\n", user, (unsigned)pw->pw_uid, tier);
    return EXIT_SUCCESS;
}

int main(int argc, char* argv[]) {
    if (argc == 2 && strcmp(argv[1], "show") == 0) return show();
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        if (send_command("reset") < 0) return EXIT_FAILURE;
        printf("all users reset to SILVER\n");
        return EXIT_SUCCESS;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) return set(argv[2], argv[3]);
    fprintf(stderr,
            "usage: tier set <user> <gold|silver|bronze>\n"
            "       tier reset\n"
            "       tier show\n");
    return EXIT_FAILURE;
}
