# Fixed-opening 2048 Solver

This directory contains the first brute-force AND-OR DFS solver described in
`../solver.txt`.

## Build

```bash
make solver
```

The executable is `solver/solver2048`. It reuses the board mechanics and the
read-only N-tuple evaluator exposed by `2048.cpp`. A raw DFS baseline remains
available, while the shipped runtime configuration enables the practical exact
TT plus D4 symmetry mode. The transposition table merges repeated `(board,
phase)` states, with separately controlled D4 symmetry canonicalization.

## Defaults and configuration

All runtime settings and the fixed opening are in `solver/solver.cfg`:

```text
opening       = 2@0, 2@1 (Player to move)
winning tile  = 8
node limit    = 0 (unlimited)
TT            = enabled
TT symmetry   = enabled
persistent TT = save result/8/8_ttfile.tt
network       = 4x6patt
model         = models/4x6patt.w
SGF output    = result/8/8_ttfile.sgf
```

After building, no command-line settings are required:

```bash
solver/solver2048
```

The executable loads `solver/solver.cfg` by default. Select another experiment
configuration with:

```bash
solver/solver2048 --config solver/target16.cfg
```

The cfg format is strict `key=value`, with blank lines, full-line comments, and
inline `# comments` supported. Unknown, duplicate, missing, or ill-typed keys
are errors. Relative model and SGF paths use the process working directory,
matching the referenced `game-solver` behavior. The complete supported file is:

```ini
winning_tile=8
node_limit=0
progress_interval=100000
use_transposition_table=true
use_tt_symmetry=true
load_tt=false
save_tt=true
tt_path=result/8/8_ttfile.tt
opening=2@0,2@1
network=4x6patt
model_path=models/4x6patt.w
sgf_path=result/8/8_ttfile.sgf
```

CLI resource flags remain optional overrides applied after cfg loading. This is
useful for one-off experiments without modifying the file.

`--node-limit 0` removes the search budget. With `--no-tt`, that is the
unlimited raw baseline. For the fixed target 8, the path-expanded search is
extremely large, so use an explicit limit for normal baseline runs.

Enable the exact transposition table with:

```bash
solver/solver2048 --winning-tile 8 --node-limit 0 --tt \
  --no-load-tt --no-save-tt
```

Enable canonical TT keys over all four rotations and four reflected rotations
with:

```bash
solver/solver2048 --winning-tile 8 --node-limit 0 --tt --tt-symmetry \
  --tt-file solver/tt/target8-symmetry.tt
```

The TT uses separate Player and Environment maps keyed by the raw 64-bit board.
It stores only exact `WIN` or `LOSS` results. A node-limit `UNKNOWN` is never
stored. TT hits are emitted to SGF as exact reference leaves pointing back to
the first trace node that proved that state, or to a persistent `(phase, key)`
when loaded from disk. Set `use_transposition_table=false` and
`use_tt_symmetry=false` in a separate cfg, or use `--no-tt`, to restore the
path-expanded baseline; `--no-tt` also disables persistent load/save.

The lookup order is winning terminal, exact TT, node limit, then
expansion. Thus an already-proven entry remains usable even when the node
budget has just been exhausted. A TT hit does not consume the expanded-node
budget or searched-action counters. With `load_tt=true`, an existing snapshot is loaded before
search; a missing file starts with empty tables. With `save_tt=true`, all loaded
and newly proven entries are streamed to `<tt_path>.tmp`, checksummed, and
atomically renamed after search. The file is tied to the winning tile, rules,
board encoding, and symmetry mode, but not to an opening or N-tuple model, so it
can be reused and extended by another fixed opening.

The v1 snapshot uses a 128-byte little-endian header, separate Player and
Environment sections, fixed 16-byte records, and a CRC64-ECMA payload checksum.
Records contain only canonical/raw board key, exact outcome, existential
witness, and legal-child count. Run-local trace IDs and action values are not
persisted. Incompatible or corrupt files are rejected before their entries are
published to the solver.

Persistent controls can be overridden without editing cfg:

```bash
solver/solver2048 --tt-file solver/tt/target16.tt --load-tt --save-tt
solver/solver2048 --no-load-tt --save-tt   # rebuild a snapshot from an empty TT
solver/solver2048 --load-tt --no-save-tt   # read-only use
```

Changing `--winning-tile` or TT symmetry while persistent load/save remains
enabled requires a different compatible `--tt-file`. For a one-off mechanism
baseline that must not read or overwrite a snapshot, use both `--no-load-tt`
and `--no-save-tt`.

At each Player node, legal slides are searched in descending action value:

```text
action value = immediate slide reward + V(afterstate)
```

The N-tuple evaluator returns only `V(afterstate)`; the solver adds the reward
reported by the shared slide API, matching the original 2048 agent's action
evaluation. Direction opcode (`up`, `right`, `down`, `left`) is the stable
tie-break. This heuristic changes DFS order only; Win/Loss still comes solely
from the complete AND-OR proof.

With `--tt-symmetry`, both store and lookup compute the minimum raw board among
all eight D4 versions and perform one hash-table operation using that canonical
key. Actions are stored in canonical orientation and mapped back on a hit, so
Player directions and Environment placement positions remain correct for the
current board. `--no-tt-symmetry` retains the exact-orientation TT as the
mechanism baseline. The solver reports exact-orientation and symmetry-derived
hits separately. `--tt-symmetry` without `--tt` is rejected.

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
reference leaf rather than duplicating its proven subtree. A trace-local hit
uses `tt_source_node_id`; a persistent hit uses `tt_source=persistent`,
`tt_phase`, and `tt_key`, because a previous process's trace ID is not valid.

## Proof checker

Build and run the independent SGF + TT verifier with:

```bash
make proof-checker
solver/proof_checker --sgf result/opening.sgf --tt result/target.tt
```

The checker reloads both artifacts, validates the TT format and SGF-to-TT
checksum binding, replays the complete retained SGF trace, and independently
verifies the root AND/OR proof DAG. At a TT leaf it follows the entry's witness
or complete child set according to the node phase and outcome until it reaches
game-rule terminals. It does not load the N-tuple evaluator or call the
solver's DFS. See `../Proof_Checker.txt` for the verification contract.

## Viewer

Open `solver/viewer/index.html` in a browser and choose the generated SGF file.
The left half displays the expanded portion of the search as an SVG node-link
graph; click a node to inspect it, use `+`/`-` to expand or collapse it, and
use the expand-all control to reveal the complete retained proof tree. Graph
nodes show their global DFS visit order; the inspector separately reports the
sibling order and whether the node was resolved by `TERMINAL`, `TT`,
`BACKPROPAGATION`, or `NODE_LIMIT`. Drag or scroll over the graph to pan and
zoom. The right half reconstructs the board from the opening and the selected
SGF action path. This
first version intentionally loads the complete file in browser memory and does
not include the multi-GB indexed viewer. Expand-all work is yielded in batches
and reports progress, but a very large SVG can still require several GB of
memory. The viewer asks for confirmation above 50,000 visible nodes; hiding
`Skipped` stubs substantially reduces the graph when only the actual DFS trace
matters.

## Tests

```bash
make solver-test
```

The suite includes the complete unlimited target-8 search with TT enabled. The
far larger unlimited raw DFS baseline (`--no-tt`) remains an opt-in benchmark.
