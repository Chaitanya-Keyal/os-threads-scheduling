# Problem 3: Sudoku conflict analyzer and repair advisor

## The task

Read a 9×9 grid from the file named in `argv[1]`. Use 27 threads to validate
every row, column and 3×3 sub-grid. If the grid is invalid, also print a
validity score, a per-cell conflict heatmap (built by 9 threads, one per
block), the cells with the highest score and the digits each broken unit is
missing. Every result returned through `pthread_exit()` must be `malloc`ed by
the worker and freed by the parent. No locks, no warnings and no leaks are
allowed.

## Input

The parser reads the file one character at a time:

* `1`-`9` is a value, `0` or `.` is an empty cell, whitespace is skipped. The
  cells may be separated by spaces, newlines or nothing at all.
* Any other character, or a cell count other than 81, prints a diagnostic
  (`expected 81 cells, found 80`) and exits with status 1.

## Units

A unit id names one row, column or sub-grid:

| unit_id | unit           | cell i of the unit       |
|---------|----------------|--------------------------|
| 0-8     | row u          | (u, i)                   |
| 9-17    | column u-9     | (i, u-9)                 |
| 18-26   | sub-grid b=u-18 | (b/3·3 + i/3, b%3·3 + i%3) |

`unit_cell(unit, i, &r, &c)` does this mapping and is used by every phase.

## Phase 1: validation (27 threads)

Each `validate_unit` thread gets a pointer to its own `struct unit_params`. It
counts how often each digit occurs (`count[0]` counts empty cells) and then:

* sets bit d of `missing_mask` if digit d never appears,
* sets bit d of `dup_mask` if digit d appears twice or more,
* marks the unit `VALID` only if there are no blanks, nothing missing and no
  duplicates,
* writes `validity[unit_id]`, the only element of that array it may touch,
* returns a `malloc`ed `struct unit_result` with `pthread_exit(res)`.

The parent collects the 27 pointers with `pthread_join(tid, &ret)`. If all
units are valid it prints `VALID`, frees the results and exits.

## Phase 2: analytics (invalid grids only)

**(a) Validity score.** The number of valid units over 27, as a percentage
with two decimals. The sample grid has 24/27 = 88.89%.

**(b) Heatmap.** The parent first copies every `dup_mask` into the global
`dup_masks[27]` and only then creates 9 `score_block` threads. Thread b scores
the 9 cells of block b:

* an empty cell scores 3,
* a cell with value v scores one point for each of its row, column and block
  in which v is duplicated: `(dup_masks[r] >> v & 1) + (dup_masks[9+c] >> v & 1) + (dup_masks[18+b] >> v & 1)`.

A cell scores 0 when its own value is unique everywhere, even if its units are
invalid for other reasons. That localises the blame to the cells that take
part in a duplication.

**(c) Hotspots.** Every cell scoring 3 is printed as `(row,col) value=v` in
row-major order, then the total.

**(d) Missing digit report.** For each invalid unit the program prints its
kind, index, missing digits and blank count. If a unit has exactly one blank
and exactly one missing digit, the answer is forced, so it prints the cell and
the digit that belongs there, e.g. `-> cell (8,1) should be 4`.

## Why no locks are needed

| Shared array | Written by | Read by |
|--------------|-----------|---------|
| `grid` | parent, before any thread | all threads |
| `validity[27]` | unit thread u writes only `validity[u]` | parent after joining |
| `dup_masks[27]` | parent, before the block threads start | block threads |
| `heatmap[9][9]` | block thread b writes only the 9 cells of block b | parent after joining |

The blocks partition the grid, so no cell is written by two threads. The
other arrays are read-only while threads run. `pthread_create` and
`pthread_join` order the parent's writes before the threads' reads and the
threads' writes before the parent's reads. `valgrind --tool=helgrind` reports
no races and `valgrind --leak-check=full` reports no leaks.

## Memory

Each Phase 1 worker `malloc`s one `unit_result` and hands it to the parent
with `pthread_exit`. The parent keeps all 27 pointers for the report and frees
them on every exit path, including the case where a worker could not allocate
and returned `NULL`.

## Trying it

```bash
cd q3_sudoku
./sudoku_analyzer grid_valid.txt      # VALID
./sudoku_analyzer grid_invalid.txt    # the sample: 88.89%, hotspot (2,0), missing {7}
./sudoku_analyzer grid_blanks.txt     # blanks written as '.', repair hints
./sudoku_analyzer grid_short.txt      # 80 cells: diagnostic, exit status 1
valgrind --leak-check=full ./sudoku_analyzer grid_invalid.txt
```
