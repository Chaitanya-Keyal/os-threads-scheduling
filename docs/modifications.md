# Making changes on the spot

The viva may ask each member to change a feature. After editing a user-space
program, run `make` in the repository root (or the `gcc` line at the top of
the file). Kernel changes need a rebuild of only the touched file: see the end
of this page.

## Problem 1: report_card.c

| Change | Where |
|--------|-------|
| Ascending instead of descending | `before()`: `ka < kb` instead of `ka > kb` |
| Ties broken by higher id | `before()`: `a->student_id > b->student_id` |
| New sort key, e.g. -4 = total marks | add `case -4:` to `key_value()` and lower the bound in the `prompt_int` call for the key |
| Another sorting algorithm | replace the loop in `sort_segment()`; it may touch only `seg[0 .. segment_size)` |
| Allow k that does not divide n | give the last thread `size + n % k` elements: set `segment_size` per thread in `main` and use `start_index + segment_size` as the segment end in `merge_segments()` |
| Print the rank in option 2 | `print_sorted()`: print `i + 1` in front |
| Menu option 3 = class average per subject | new branch in `menu()` looping over `students[]` |
| Different file name | the `"input.txt"` default in `main` |
| Show the median too | the struct is fixed by the spec, so compute it in `print_student()` from a sorted copy of `subject_marks` |

## Problem 2: rt_bench.h, cpu_bound.c, io_bound.c, run_all.sh

| Change | Where |
|--------|-------|
| Number of children | `NUM_CHILDREN` in `rt_bench.h` |
| Other quantum values | `QUANTA` in `run_all.sh` |
| Other RT priority | `PRIO` in `run_all.sh` |
| Single runs instead of median of 3 | `RUNS=1` in `run_all.sh` |
| Also print the minimum response time | track `min_resp` next to `max_resp` in `run_bench()` and print it |
| Print each child's times | `printf` inside the `read()` loop of `run_bench()` (add an `int id` to `struct child_report` and set it in the child) |
| Mixed workload (half CPU, half I/O) | a third program whose task calls `cpu_task(id)` for even ids and `io_task(id)` for odd ones |
| Run under SCHED_OTHER for comparison | run `./cpu_bound` without `chrt`; the header shows `SCHED_OTHER` |

## Problem 3: sudoku_analyzer.c

| Change | Where |
|--------|-------|
| Print blanks as 0 in the echo | `print_matrix(..., grid, 0)` in `main` |
| Different hotspot threshold (score ≥ 2) | `heatmap[r][c] == 3` in `print_hotspots()` |
| Also list duplicated digits | in `print_missing()`, loop over `res->dup_mask` like `missing_mask` |
| Score a blank cell differently | the `v == 0` branch in `score_block()` |
| Print the per-unit verdict table | loop over `validity[]` after the joins in `main` |
| Accept another blank character, e.g. `_` | the `ch != '.'` test and the `ch == '.'` mapping in `read_grid()` |
