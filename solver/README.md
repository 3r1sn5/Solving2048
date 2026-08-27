# Fixed-opening 2048 Solver

This directory contains the first brute-force AND-OR DFS solver described in
`../solver.txt`.

## Build

```bash
make solver
```

The executable is `solver/solver2048`. It reuses the board mechanics and the
read-only N-tuple evaluator exposed by `2048.cpp`. The default mode remains the
raw DFS baseline. An optional exact transposition table can merge repeated
`(board, phase)` states; it does not use symmetry reduction.

## Defaults and configuration

Compile-time defaults and the fixed opening are in `solver/config.h`:

```text
opening       = 2@0, 2@1 (Player to move)
winning tile  = 8
node limit    = 100000
TT            = disabled
network       = 4x6patt
model         = models/4x6patt.w
SGF output    = solver/tree.sgf
```

Resource settings can be overridden on the command line:

```bash
solver/solver2048 \
  --winning-tile 8 \
  --node-limit 100000 \
  --no-tt \
  --model models/4x6patt.w \
  --sgf solver/tree.sgf
```

`--node-limit 0` removes the search budget. With `--no-tt`, that is the
unlimited raw baseline. For the fixed target 8, the path-expanded search is
extremely large, so use an explicit limit for normal baseline runs.

Enable the exact transposition table with:

```bash
solver/solver2048 --winning-tile 8 --node-limit 0 --tt
```

The TT uses separate Player and Environment maps keyed by the raw 64-bit board.
It stores only exact `WIN` or `LOSS` results. A node-limit `UNKNOWN` is never
stored. TT hits are emitted to SGF as exact reference leaves pointing back to
the first trace node that proved that state. `--no-tt` restores the unchanged
path-expanded baseline and is the compile-time default in `solver/config.h`.

The lookup order is winning terminal, TT, node limit, then expansion. Thus an
already-proven entry remains usable even when the node budget has just been
exhausted. A TT hit does not consume the expanded-node budget or count as a
node-limit cutoff. Both tables are cleared at the beginning of every `solve()`;
they are neither persisted to disk nor shared across solver runs.

Run `solver/solver2048 --help` for all options.

## Results

All outcomes use the Player perspective:

- `WIN`: Player has a slide strategy that reaches the target against every
  legal Environment placement.
- `LOSS`: Environment has a placement strategy that prevents the target.
- `UNKNOWN`: the node limit prevented a complete proof.

The SGF records every visited DFS child. Actions not called because an AND/OR
node already had a decisive result are recorded separately as `Skipped`.
With TT enabled, a repeated state is recorded as a `TRANSPOSITION_TABLE`
reference leaf rather than duplicating its proven subtree.

## Viewer

Open `solver/viewer/index.html` in a browser and choose the generated SGF file.
The left half displays the expanded portion of the search as an SVG node-link
graph; click a node to inspect it, use `+`/`-` to expand or collapse it, and
use the expand-all control to reveal the complete retained proof tree. Graph
nodes show their global DFS visit order; the inspector separately reports the
sibling order. Drag or scroll over the graph to pan and zoom. The right half
reconstructs the board from the opening and the selected SGF action path. This
first version intentionally loads the complete file in browser memory and does
not include the multi-GB indexed viewer. Expand-all work is yielded in batches
and reports progress, but a very large SVG can still require several GB of
memory. The viewer asks for confirmation above 50,000 visible nodes; hiding `Skipped`
stubs substantially reduces the graph when only the actual DFS trace matters.

## Tests

```bash
make solver-test
```

The suite includes the complete unlimited target-8 search with TT enabled. The
far larger unlimited raw DFS baseline (`--no-tt`) remains an opt-in benchmark.
