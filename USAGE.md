# Usage

## Build

```
make          # builds ./adiff
make test     # self-test + verified diff of the examples
make clean
```

Requirements: a C11 compiler (gcc or clang) and `make`. On Windows use WSL (or MSYS2). `adiff bench` needs `fork` and `wait4`, so it runs on Linux, macOS or WSL only.

## Diff two files

```
./adiff [options] OLD NEW
```

Exit status: 0 = files are the same, 1 = files differ, 2 = error.

| Option | Meaning |
|---|---|
| `--engine E` | `auto` (default), `classic`, `full`, `hirschberg`, `banded`, `myers` |
| `--no-moves` | do not detect moved blocks |
| `--no-tokens` | do not mark changed tokens inside changed lines |
| `--move-min N` | smallest block reported as a move (default 3 lines) |
| `--context N` | lines of context around a change (default 3) |
| `--color` | ANSI colors |
| `--stats` | engine used, sizes, time and peak memory (to stderr) |
| `--verify` | check that old file + script == new file |

### Output legend

```
 text        unchanged line
-text        deleted line
+text        inserted line
<text        line moved OUT of this place
>text        line moved IN to this place
[-x-]  {+y+} the tokens that changed inside a modified line
```

A header lists every moved block with its old and new line numbers.

### Engines

| Name | What it is | Time | Space |
|---|---|---|---|
| `classic` | textbook full table, no trimming (baseline) | O(mn) | O(mn) |
| `full` | full table after trimming common head and tail | O(mn) | O(mn) |
| `hirschberg` | divide and conquer, two DP rows only | O(mn) | O(min(m, n)) |
| `banded` | DP restricted to a band that doubles until the result is provably optimal | O(kn) | O(kn) |
| `myers` | greedy O(ND) algorithm, as in git diff (baseline) | O(ND) | O(D²) here |
| `auto` | trims, then picks full, banded or Hirschberg | see selector | |

Every engine returns an optimal answer, so they must all report the same number of common lines.

### The `auto` selector

After trimming the common head and tail, with m' and n' lines left:

1. If (m'+1) × (n'+1) is at most 4,000,000 cells: full table.
2. Else, if a cheap lower bound on the edit distance is at most 10% of the longer side: try banded DP (start band 64, doubling, capped at 10% and at 256 MB). If it cannot finish inside the caps, fall through.
3. Otherwise: Hirschberg.

The constants are at the top of `src/lcs.c` (`AUTO_FULL_CELLS`, `AUTO_BAND_PERCENT`, `BAND_START_K`, `BAND_MEM_LIMIT`).

## Benchmark

```
./adiff bench [--runs N] [--engines classic,full,hirschberg,banded,myers,auto] OLD NEW
```

Every measurement runs in its own child process. A baseline child that only loads the files is measured first and subtracted, so "extra mem" is the memory used by the algorithm itself. Time is the median of the runs and covers only the LCS computation.

## Make test data

```
./adiff gen --lines 12000 --edit-rate 0.05 --moves 4 --seed 7 old.txt new.txt
```

Writes pseudo source code with realistic repetition (blank lines, braces, repeated statements), then edits it (deletions, changed numbers, insertions) and relocates `--moves` blocks.

## Self-test

```
./adiff selftest
```

Runs about 3,000 random cases with small alphabets (many ties), mutated sequences with moved blocks, and large inputs that exercise the banded and Hirschberg paths of the selector. It checks that every engine returns a valid, increasing list of matches and that all engines agree on the LCS length.
