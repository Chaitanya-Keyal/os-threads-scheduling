# Documentation

These notes explain the four problems in plain language: what is being asked,
which operating-system ideas are involved, how the code solves it and what to
show in the demo. **Start with `viva-prep.md`**; it gives the reading order
and the questions every member should be able to answer.

| File | What it covers |
|------|----------------|
| `viva-prep.md` | Read this first: what each member must know, 60-second summaries, likely questions with answers |
| `concepts.md` | The building blocks: pthreads, lock-free partitioning, RT scheduling, context switches, the fair scheduler and nice, process groups, kernel_clone, commit_creds, procfs, sysctl |
| `01-report-card.md` | Problem 1: threaded aggregation, per-segment sorting and the k-way merge |
| `02-rt-scheduling.md` | Problem 2: the measurement programs and what the results show |
| `03-sudoku.md` | Problem 3: 27 validators, 9 heatmap threads, hotspots and repair hints |
| `04-fork-guard.md` | Problem 4: the fork-storm ladder, CPU tiers and the design decisions |
| `demo.md` | Step-by-step demo script for all four problems |
| `modifications.md` | Where to edit for the changes likely to be asked in the viva |
