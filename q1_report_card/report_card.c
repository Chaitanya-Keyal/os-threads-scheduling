/*
 * CS F372 Assignment 2, Problem 1
 *
 * Multithreaded student report card generator.
 *
 * Phase 1: one worker thread per student fills that student's entry of
 *          students[] (average, minimum, maximum, copy of the marks).
 * Phase 2: the user picks a sort key and k. k worker threads each sort one
 *          contiguous segment of students[] in place; the parent then does a
 *          k-way merge of the segments into sorted[].
 * Phase 3: an interactive menu over the results. No threads are created.
 *
 * No locks are needed because every thread writes only to its own part of
 * the shared arrays, and everything a thread reads is written before the
 * thread is created.
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o report_card report_card.c
 * Usage: ./report_card [marks_file]      (default: input.txt)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SUBJECTS 32
#define MAX_STUDENTS 128

typedef struct {
    int student_id;
    double average;
    int minimum;
    int maximum;
    int subject_marks[MAX_SUBJECTS];  // MAX_SUBJECTS: 32, MAX_STUDENTS: 128
} student_stats;

struct thread_params {
    int thread_id;     // Logical thread ID
    int start_index;   // Starting index in student_stats array
    int segment_size;  // Number of elements to process
};

/* Written by the parent before any thread exists, read-only afterwards. */
static int num_subjects, num_students;
static int marks[MAX_SUBJECTS][MAX_STUDENTS]; /* marks[subject][student] */
static int sort_key; /* -3 max, -2 min, -1 avg, or a subject */

/* Phase 1: worker i writes only students[i]. Phase 2: each sorter touches
 * only its own segment. */
static student_stats students[MAX_STUDENTS];

/* Phase 2 result, written by the parent after all sorters are joined. */
static student_stats sorted[MAX_STUDENTS];

/* Reads the next integer, skipping "// ..." comments like the ones in the
 * example input of the problem statement. */
static int next_int(FILE* f, int* value) {
    int c;

    while (fscanf(f, "%d", value) != 1) {
        if (fgetc(f) != '/') return 0; /* EOF or a stray character */
        while ((c = fgetc(f)) != '\n' && c != EOF);
    }
    return 1;
}

static int read_marks(const char* path) {
    FILE* f = fopen(path, "r");

    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (!next_int(f, &num_subjects) || !next_int(f, &num_students) ||
        num_subjects < 1 || num_subjects > MAX_SUBJECTS || num_students < 1 ||
        num_students > MAX_STUDENTS) {
        fprintf(stderr, "%s: need 1-%d subjects and 1-%d students\n", path,
                MAX_SUBJECTS, MAX_STUDENTS);
        fclose(f);
        return -1;
    }
    for (int i = 0; i < num_subjects; i++) {
        for (int j = 0; j < num_students; j++) {
            if (!next_int(f, &marks[i][j])) {
                fprintf(stderr, "%s: expected %d x %d marks\n", path,
                        num_subjects, num_students);
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

/* Phase 1 worker: aggregates column *arg of the marks matrix. */
static void* aggregate(void* arg) {
    int id = *(int*)arg;
    student_stats* s = &students[id];
    int total = 0;

    s->student_id = id;
    s->minimum = s->maximum = marks[0][id];
    for (int i = 0; i < num_subjects; i++) {
        int m = marks[i][id];

        s->subject_marks[i] = m;
        total += m;
        if (m < s->minimum) s->minimum = m;
        if (m > s->maximum) s->maximum = m;
    }
    s->average = (double)total / num_subjects;
    pthread_exit(NULL);
}

static double key_value(const student_stats* s) {
    switch (sort_key) {
        case -3:
            return s->maximum;
        case -2:
            return s->minimum;
        case -1:
            return s->average;
        default:
            return s->subject_marks[sort_key];
    }
}

/* Descending on the key; ties go to the lower student_id. Used by the
 * sorting threads and by the parent's merge, so both apply the same rule. */
static int before(const student_stats* a, const student_stats* b) {
    double ka = key_value(a), kb = key_value(b);

    return ka > kb || (ka == kb && a->student_id < b->student_id);
}

/* Phase 2 worker: insertion sort of students[start .. start + size). */
static void* sort_segment(void* arg) {
    struct thread_params* p = arg;
    student_stats* seg = &students[p->start_index];

    for (int i = 1; i < p->segment_size; i++) {
        student_stats cur = seg[i];
        int j = i - 1;

        while (j >= 0 && before(&cur, &seg[j])) {
            seg[j + 1] = seg[j];
            j--;
        }
        seg[j + 1] = cur;
    }
    pthread_exit(NULL);
}

/* k-way merge: repeatedly take the best head among the k sorted segments. */
static void merge_segments(int k, int size) {
    int head[MAX_STUDENTS]; /* next unmerged index of each segment */

    for (int t = 0; t < k; t++) head[t] = t * size;
    for (int i = 0; i < num_students; i++) {
        int best = -1;

        for (int t = 0; t < k; t++) {
            if (head[t] == (t + 1) * size) continue; /* segment used up */
            if (best < 0 || before(&students[head[t]], &students[head[best]]))
                best = t;
        }
        sorted[i] = students[head[best]++];
    }
}

/* Prompts until a line holding one integer in [lo, hi] is entered. */
static int prompt_int(const char* prompt, int lo, int hi) {
    char line[128];
    int value;

    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) exit(EXIT_SUCCESS);
        if (sscanf(line, "%d", &value) == 1 && value >= lo && value <= hi)
            return value;
        printf("Please enter a number between %d and %d.\n", lo, hi);
    }
}

static void run_threads(void* (*fn)(void*), void* args, size_t arg_size,
                        int count) {
    pthread_t tids[MAX_STUDENTS];

    for (int i = 0; i < count; i++) {
        if (pthread_create(&tids[i], NULL, fn, (char*)args + i * arg_size)) {
            fprintf(stderr, "pthread_create failed\n");
            exit(EXIT_FAILURE);
        }
    }
    for (int i = 0; i < count; i++) pthread_join(tids[i], NULL);
}

static void print_student(int id) {
    const student_stats* s = &students[0];

    /* students[] was reordered by the sorters, so look the id up. */
    for (int i = 0; i < num_students; i++)
        if (students[i].student_id == id) s = &students[i];
    printf("Student ID   : %d\n", s->student_id);
    printf("Average      : %.2f\n", s->average);
    printf("Minimum      : %d\n", s->minimum);
    printf("Maximum      : %d\n", s->maximum);
    printf("Subject Marks: [");
    for (int i = 0; i < num_subjects; i++)
        printf("%s%d", i ? ", " : "", s->subject_marks[i]);
    printf("]\n");
}

static void print_sorted(void) {
    for (int i = 0; i < num_students; i++) {
        printf("s%-4d", sorted[i].student_id);
        if (sort_key == -1)
            printf("%.2f\n", sorted[i].average);
        else
            printf("%d\n", (int)key_value(&sorted[i]));
    }
}

static void menu(void) {
    char line[128];
    int cmd, id;

    printf("--- Report Card Menu ---\n");
    for (;;) {
        printf(
            "Enter command (1 <id> = view student, 2 = view all, 0 = exit): ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) return;
        int n = sscanf(line, "%d %d", &cmd, &id);

        if (n >= 1 && cmd == 0) return;
        if (n == 1 && cmd == 2)
            print_sorted();
        else if (n == 2 && cmd == 1 && id >= 0 && id < num_students)
            print_student(id);
        else if (n == 2 && cmd == 1)
            printf("No student with id %d (valid: 0..%d).\n", id,
                   num_students - 1);
        else
            printf("Unknown command.\n");
    }
}

int main(int argc, char* argv[]) {
    int ids[MAX_STUDENTS];
    struct thread_params params[MAX_STUDENTS];
    char prompt[128];
    int k, size;

    if (read_marks(argc > 1 ? argv[1] : "input.txt") < 0) return EXIT_FAILURE;

    /* Phase 1: aggregation, one thread per student. */
    for (int i = 0; i < num_students; i++) ids[i] = i;
    run_threads(aggregate, ids, sizeof(ids[0]), num_students);

    /* Phase 2: k threads sort k segments, then the parent merges them. */
    snprintf(prompt, sizeof(prompt),
             "Enter sort key (-3=max, -2=min, -1=avg, 0..%d=subject index): ",
             num_subjects - 1);
    sort_key = prompt_int(prompt, -3, num_subjects - 1);
    for (;;) {
        k = prompt_int("Enter number of sorting threads (k): ", 1,
                       num_students);
        if (num_students % k == 0) break;
        printf("k must divide the number of students (%d).\n", num_students);
    }
    size = num_students / k;
    for (int t = 0; t < k; t++) {
        params[t].thread_id = t;
        params[t].start_index = t * size;
        params[t].segment_size = size;
    }
    run_threads(sort_segment, params, sizeof(params[0]), k);
    merge_segments(k, size);

    /* Phase 3: queries, no threads. */
    menu();
    return EXIT_SUCCESS;
}
