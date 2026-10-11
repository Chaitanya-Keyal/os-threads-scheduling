/*
 * CS F372 Assignment 2, Problem 3
 *
 * Sudoku conflict analyzer and repair advisor.
 *
 * Phase 1: 27 threads validate the 9 rows (unit 0-8), 9 columns (9-17) and
 *          9 sub-grids (18-26). Each writes its verdict to validity[unit] and
 *          returns a malloc()ed unit_result through pthread_exit().
 * Phase 2: (only for an invalid grid) validity score, a conflict heatmap
 *          filled by 9 threads (one per 3x3 block), hotspots and a missing
 *          digit report.
 *
 * Race freedom comes from partitioning, not locks: grid[] and dup_masks[]
 * are written by the parent before the threads that read them are created,
 * validity[u] is written only by unit thread u, and each block thread writes
 * only the 9 heatmap cells of its own block.
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o sudoku_analyzer sudoku_analyzer.c
 * Usage: ./sudoku_analyzer <grid_file>
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define VALID 1
#define INVALID 0
#define NUM_UNITS 27

struct unit_params {
    int unit_id; /* 0-26 */
};

struct unit_result {
    int unit_id;
    int valid;        /* 1 = VALID, 0 = INVALID */
    int missing_mask; /* bit d set  => digit d never appears */
    int dup_mask;     /* bit d set  => digit d appears twice or more*/
    int blank_count;  /* number of empty cells in this unit */
};

struct block_params {
    int block_id; /* 0-8, row-major */
};

static int grid[9][9];           /* 0 = empty cell */
static int validity[NUM_UNITS];  /* validity[u] written only by thread u */
static int dup_masks[NUM_UNITS]; /* copied by the parent before Phase 2 */
static int heatmap[9][9];        /* block thread b writes only its block */

static const char* unit_kind[] = {"Row", "Column", "Sub-grid"};

/* Row/column of the i-th cell (0-8) of a unit. */
static void unit_cell(int unit, int i, int* r, int* c) {
    int b = unit - 18;

    if (unit < 9) {
        *r = unit;
        *c = i;
    } else if (unit < 18) {
        *r = i;
        *c = unit - 9;
    } else {
        *r = b / 3 * 3 + i / 3;
        *c = b % 3 * 3 + i % 3;
    }
}

/* Reads 81 cells: digits 1-9, and 0 or '.' for an empty cell. Whitespace
 * between cells is optional. */
static int read_grid(const char* path) {
    FILE* f = fopen(path, "r");
    int ch, n = 0;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    while ((ch = fgetc(f)) != EOF) {
        if (isspace(ch)) continue;
        if (ch != '.' && !isdigit(ch)) {
            fprintf(stderr, "%s: invalid character '%c'\n", path, ch);
            fclose(f);
            return -1;
        }
        if (n < 81) grid[n / 9][n % 9] = ch == '.' ? 0 : ch - '0';
        n++;
    }
    fclose(f);
    if (n != 81) {
        fprintf(stderr, "%s: expected 81 cells, found %d\n", path, n);
        return -1;
    }
    return 0;
}

/* Phase 1 worker: counts the digits of one unit. */
static void* validate_unit(void* arg) {
    int id = ((struct unit_params*)arg)->unit_id;
    struct unit_result* res = malloc(sizeof(*res));
    int count[10] = {0}; /* count[0] = empty cells */
    int r, c;

    if (res == NULL) pthread_exit(NULL);
    for (int i = 0; i < 9; i++) {
        unit_cell(id, i, &r, &c);
        count[grid[r][c]]++;
    }
    res->unit_id = id;
    res->missing_mask = 0;
    res->dup_mask = 0;
    res->blank_count = count[0];
    for (int d = 1; d <= 9; d++) {
        if (count[d] == 0) res->missing_mask |= 1 << d;
        if (count[d] > 1) res->dup_mask |= 1 << d;
    }
    res->valid =
        res->blank_count == 0 && res->missing_mask == 0 && res->dup_mask == 0
            ? VALID
            : INVALID;
    validity[id] = res->valid;
    pthread_exit(res);
}

/* Phase 2 worker: conflict scores for the 9 cells of one 3x3 block. */
static void* score_block(void* arg) {
    int b = ((struct block_params*)arg)->block_id;
    int r, c, v;

    for (int i = 0; i < 9; i++) {
        unit_cell(18 + b, i, &r, &c);
        v = grid[r][c];
        if (v == 0)
            heatmap[r][c] = 3;
        else
            heatmap[r][c] = (dup_masks[r] >> v & 1) +
                            (dup_masks[9 + c] >> v & 1) +
                            (dup_masks[18 + b] >> v & 1);
    }
    pthread_exit(NULL);
}

static void print_matrix(const char* title, int m[9][9], int blank_as_dot) {
    printf("%s\n", title);
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (blank_as_dot && m[r][c] == 0)
                printf(c ? " ." : ".");
            else
                printf(c ? " %d" : "%d", m[r][c]);
        }
        printf("\n");
    }
    printf("\n");
}

static void print_hotspots(void) {
    int total = 0;

    printf("Hotspots (score 3):\n");
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (heatmap[r][c] == 3) {
                printf("  (%d,%d) value=%d\n", r, c, grid[r][c]);
                total++;
            }
        }
    }
    printf("Total Hotspots: %d\n\n", total);
}

static void print_missing(struct unit_result* results[]) {
    printf("Missing Digit Report:\n");
    for (int u = 0; u < NUM_UNITS; u++) {
        const struct unit_result* res = results[u];
        int first = 1, digit = 0, r, c;

        if (res->valid) continue;
        printf("  %-8s %d : missing {", unit_kind[u / 9], u % 9);
        for (int d = 1; d <= 9; d++) {
            if (res->missing_mask >> d & 1) {
                printf(first ? "%d" : ",%d", d);
                first = 0;
                digit = digit ? -1 : d; /* -1: more than one missing */
            }
        }
        printf("}  blanks=%d\n", res->blank_count);

        /* Repair advisor: one blank and one missing digit pin the answer. */
        if (res->blank_count == 1 && digit > 0) {
            for (int i = 0; i < 9; i++) {
                unit_cell(u, i, &r, &c);
                if (grid[r][c] == 0)
                    printf("      -> cell (%d,%d) should be %d\n", r, c, digit);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    pthread_t tids[NUM_UNITS];
    struct unit_params units[NUM_UNITS];
    struct block_params blocks[9];
    struct unit_result* results[NUM_UNITS];
    int valid_units = 0, failed = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <grid_file>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (read_grid(argv[1]) < 0) return EXIT_FAILURE;
    print_matrix("Input Grid:", grid, 1);

    /* Phase 1: validation. */
    for (int u = 0; u < NUM_UNITS; u++) {
        units[u].unit_id = u;
        if (pthread_create(&tids[u], NULL, validate_unit, &units[u])) {
            fprintf(stderr, "pthread_create failed\n");
            return EXIT_FAILURE;
        }
    }
    for (int u = 0; u < NUM_UNITS; u++) {
        void* ret;

        pthread_join(tids[u], &ret);
        results[u] = ret;
        if (results[u] == NULL) failed = 1;
    }
    if (failed) {
        fprintf(stderr, "a validation thread ran out of memory\n");
        for (int u = 0; u < NUM_UNITS; u++) free(results[u]);
        return EXIT_FAILURE;
    }
    for (int u = 0; u < NUM_UNITS; u++) valid_units += validity[u];

    if (valid_units == NUM_UNITS) {
        printf("VALID\n");
    } else {
        printf("INVALID\n");

        /* (a) validity score */
        printf("Validity Score: %.2f%%\n\n", 100.0 * valid_units / NUM_UNITS);

        /* (b) heatmap: dup_masks[] is filled before the block threads
         * start, so they only ever read it. */
        for (int u = 0; u < NUM_UNITS; u++) dup_masks[u] = results[u]->dup_mask;
        for (int b = 0; b < 9; b++) {
            blocks[b].block_id = b;
            if (pthread_create(&tids[b], NULL, score_block, &blocks[b])) {
                fprintf(stderr, "pthread_create failed\n");
                return EXIT_FAILURE;
            }
        }
        for (int b = 0; b < 9; b++) pthread_join(tids[b], NULL);
        print_matrix("Conflict Density Heatmap:", heatmap, 0);

        /* (c) hotspots and (d) missing digits */
        print_hotspots();
        print_missing(results);
    }

    for (int u = 0; u < NUM_UNITS; u++) free(results[u]);
    return EXIT_SUCCESS;
}
