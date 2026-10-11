# Problem 1: Multithreaded report card generator

## The task

`input.txt` holds a marks matrix: m subjects (rows) by n students (columns).
The program must:

1. **Aggregate** with one thread per student: average, minimum, maximum and a
   copy of the marks go into that student's `student_stats` entry.
2. **Sort** with k threads: the user picks a key (max, min, average or one
   subject) and k. Each thread sorts one contiguous segment in descending
   order, and the parent k-way merges the segments into a new array.
3. **Answer queries** from a menu without creating any threads.

Only `pthread_create/join/exit/self/detach/cancel` may be used. Mutexes,
semaphores and condition variables are not allowed.

## Data

```c
static int marks[MAX_SUBJECTS][MAX_STUDENTS];   /* marks[subject][student] */
static student_stats students[MAX_STUDENTS];    /* Phase 1 output, sorted in place in Phase 2 */
static student_stats sorted[MAX_STUDENTS];      /* Phase 2 output (the merge) */
static int sort_key;                            /* -3 max, -2 min, -1 avg, else a subject */
```

`student_stats` and `struct thread_params` are copied from the problem
statement unchanged.

## Phase 1: aggregation

`main` fills `ids[i] = i` and starts one `aggregate` thread per student with
`&ids[i]` as the argument. Each thread passes its own address, because a
pointer to a loop variable would change under the thread. Thread i reads
column i of `marks` and writes `students[i]` only, then calls `pthread_exit`.
The parent joins all n threads before going on.

## Phase 2: sorting and the k-way merge

The key and k are read with `prompt_int`, which asks again on bad input. k must
be between 1 and n and divide n. The parent fills one `thread_params` per
thread (`start_index = t * n/k`, `segment_size = n/k`) and starts the
`sort_segment` threads.

Each thread insertion-sorts `students[start .. start+size)` in place. Every
comparison goes through `before(a, b)`:

```c
return ka > kb || (ka == kb && a->student_id < b->student_id);
```

That puts higher keys first and breaks ties by the lower id. The parent's merge
uses the same function, so the threads and the parent apply the same rule, as
the problem requires.

After joining, `merge_segments` keeps a "head" index per segment. For each of
the n output positions it scans the k heads, takes the best one with
`before()`, copies it into `sorted[i]` and advances that head. This is
O(n·k), which is fine for at most 128 students. A heap would give O(n log k).

Insertion sort is used because segments hold at most 128 elements, it is
stable and simple, and the problem lets us pick any algorithm.

## Phase 3: the menu

* `1 <id>`: prints the student's id, average, minimum, maximum and marks.
  `students[]` was reordered in Phase 2, so the student is looked up by
  `student_id`.
* `2`: prints `sorted[]` as `s<id>` and the value of the active key. The
  average prints with two decimals and the integer keys as integers.
* `0` or end of input: exit.

## Why no locks are needed

| Shared data | Written by | When | Read by |
|-------------|-----------|------|---------|
| `marks`, `num_subjects`, `num_students` | parent | before any thread | Phase 1 threads |
| `students[i]` | Phase 1 thread i only | Phase 1 | parent after join, Phase 2 |
| `students[start..start+size)` | sort thread t only | Phase 2 | parent after join |
| `sort_key` | parent | before the sort threads start | sort threads, merge |
| `sorted` | parent | after all sort threads are joined | menu |

No two threads ever write the same element, and nothing a thread reads is
written while it runs. `pthread_create` makes the parent's earlier writes
visible to the new thread, and `pthread_join` makes the thread's writes visible
to the parent. Those two calls provide all the ordering the program needs.
`valgrind --tool=helgrind` reports no races.

## Input handling

* The file is `input.txt` by default, or the first argument if one is given.
* `// ...` comments like the ones in the problem's example are skipped, so the
  example can be pasted verbatim.
* Bad counts (more than 32 subjects or 128 students), too few marks or a
  missing file print an error and exit with status 1.
* An invalid key, k or student id is reported and asked again.

## Trying it

```bash
cd q1_report_card && ./report_card
Enter sort key (-3=max, -2=min, -1=avg, 0..3=subject index): -1
Enter number of sorting threads (k): 3
```

Then `1 2`, `2` and `0` reproduce the sample output exactly.
