# Documentation

These notes explain the four problems in plain language: what is being asked,
which operating-system ideas are involved, how the code solves it and what to
look at during the demo. Read `concepts.md` first if threads, real-time
scheduling or kernel hooks are new to you.

| File | What it covers |
|------|----------------|
| `concepts.md` | The building blocks: pthreads, lock-free partitioning, RT scheduling, context switches, the fair scheduler and nice, process groups, kernel_clone, commit_creds, procfs, sysctl |
| `01-report-card.md` | Problem 1: threaded aggregation, per-segment sorting and the k-way merge |
| `02-rt-scheduling.md` | Problem 2: the measurement programs and what the results show |
| `03-sudoku.md` | Problem 3: 27 validators, 9 heatmap threads, hotspots and repair hints |
| `04-fork-guard.md` | Problem 4: the fork-storm ladder, CPU tiers and the design decisions |
| `q4-evidence/` | Problem 4: output of test cases 1-8 captured on the VM |
| `header.pdf`, `q4-help-doc.pdf` | The handouts: workload headers for Problem 2 and the kernel help document for Problem 4 |
| `demo.md` | Step-by-step demo script for all four problems |
| `modifications.md` | Where to edit for the changes likely to be asked in the viva |
