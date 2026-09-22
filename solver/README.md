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
persistent TT = save result/RZ/8_RZ.tt
RZone         = lookup, save result/RZ/8_RZ.rz
network       = 4x6patt
model         = models/4x6patt.w
SGF output    = result/RZ/8_RZ.sgf
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
are errors. `rzone_active_pruning` is a backward-compatible optional key and
defaults to `false` when omitted. Relative
model and SGF paths use the process working directory, matching the referenced
`game-solver` behavior. The complete supported file is:

```ini
winning_tile=8
node_limit=0
progress_interval=100000
use_transposition_table=true
use_tt_symmetry=true
load_tt=false
save_tt=true
tt_path=result/RZ/8_RZ.tt
rzone_mode=lookup
rzone_lookup_policy=smallest_first
use_rzone_symmetry=true
rzone_max_cells=16
rzone_shadow_exact_check=false
rzone_active_pruning=false
load_rz=false
save_rz=true
rz_path=result/RZ/8_RZ.rz
opening=2@0,2@1
network=4x6patt
model_path=models/4x6patt.w
sgf_path=result/RZ/8_RZ.sgf
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
Like RZone, it stores only proven `WIN` results derived upward from winning
terminals. `LOSS` and node-limit `UNKNOWN` still propagate through the search
but are never cached. TT hits are emitted to SGF as exact Win reference leaves
pointing back to the first trace node that proved that state, or to a persistent
`(phase, key)` when loaded from disk. Set `use_transposition_table=false` and
`use_tt_symmetry=false` in a separate cfg, or use `--no-tt`, to restore the
path-expanded baseline; `--no-tt` also disables persistent load/save.

The lookup order is winning terminal, RZone, exact TT, node limit, then
expansion. Thus an already-proven entry remains usable even when the node
budget has just been exhausted. `player_nodes` and
`environment_nodes` count every node entered by DFS, including terminal, TT,
RZone, and node-limit leaves. A sibling resolved by parent-level active pruning
is not visited and therefore is excluded, even though its proof-compatible
`RZONE_TT` leaf remains in SGF. The node limit and progress interval use this
visited-node count. New SGFs declare `node_count_semantics=visited`. With
`load_tt=true`, an
existing snapshot is loaded before search; a missing file starts with empty
tables. With `save_tt=true`, all loaded
and newly proven entries are streamed to `<tt_path>.tmp`, checksummed, and
atomically renamed after search. The file is tied to the winning tile, rules,
board encoding, and symmetry mode, but not to an opening or N-tuple model, so it
can be reused and extended by another fixed opening.

The v1 snapshot uses a 128-byte little-endian header with a required `WIN_ONLY`
feature flag, separate Player and
Environment sections, fixed 16-byte records, and a CRC64-ECMA payload checksum.
Every record has a fixed Win outcome; Player records carry an existential slide
witness, while Environment Win records have no witness. Records also preserve
the canonical/raw board key and legal-child count. Run-local trace IDs and action
values are not persisted. Incompatible or corrupt files are rejected before
their entries are published to the solver.

Persistent controls can be overridden without editing cfg:

```bash
solver/solver2048 --tt-file solver/tt/target16.tt --load-tt --save-tt
solver/solver2048 --no-load-tt --save-tt   # rebuild a snapshot from an empty TT
solver/solver2048 --load-tt --no-save-tt   # read-only use
```

RZone snapshots use parallel controls:

```bash
solver/solver2048 --rz-file solver/rz/target16.rz --load-rz --save-rz
solver/solver2048 --no-load-rz --save-rz   # rebuild from an empty RZone table
solver/solver2048 --load-rz --no-save-rz   # read-only use
```

The v1 `.rz` snapshot has a 128-byte little-endian header, child-first
32-byte certificate records, and a CRC64-ECMA payload checksum. Loading
strictly validates the complete certificate DAG before publishing any record.
`rzone_max_cells` only limits which loaded patterns enter the lookup index;
all records and IDs remain available and are preserved when the file is saved
again.

Changing `--winning-tile` or TT symmetry while persistent load/save remains
enabled requires a different compatible `--tt-file`. For a one-off mechanism
baseline that must not read or overwrite a snapshot, use both `--no-load-tt`
and `--no-save-tt`.

### First RZone implementation

`rzone_mode=generate` derives immutable Player-Win certificates during normal
backpropagation but never looks them up. It is the control run for measuring
certificate-generation overhead: exact-TT node and action counts remain the
same as the RZone-disabled baseline.

`rzone_mode=lookup` checks the in-memory RZone table before the exact TT. A
matching partial board pattern returns a Win proof leaf without replay and
without generating that node's actions. Certificates are built by
three local rules: a one-cell winning terminal, a fixed winning Player slide
whose source preimage is sufficient to determine the relevant output, and an
Environment universal-placement step whose relevant cells are all non-empty.
The last condition guarantees every legal 2/4 placement stays outside the
zone. `rzone_max_cells` bounds accepted certificates; certificate depth is
recorded for diagnostics and tie-breaking but does not limit derivation.

`rzone_lookup_policy=full_scan` preserves the original policy: scan every
indexed mask and choose the globally smallest-cell certificate, then shortest
depth and oldest ID. `smallest_first` groups masks by cell count, checks groups
from one to sixteen cells, completes the depth/ID/transform tie-break within
the first matching cell-count group, and then returns. It guarantees minimum
cell count because every smaller group has already missed. Each indexed mask also stores its pre-expanded
64-bit nibble mask, so lookup performs one AND instead of rebuilding it from
sixteen cells.

`use_rzone_symmetry=true` (or `--rzone-symmetry`) computes all eight D4 versions
of the candidate board and searches the existing one-copy index. The returned
transform maps the current board into the stored certificate orientation;
Player slide witnesses are mapped back before use. Parent lifting is performed
in that same stored orientation, so `.rz` v1 needs no new edge field and lookup
still performs no certificate replay. The SGF records the certificate ID and
numeric `rzone_transform`; a nonzero transform identifies a symmetry hit and is
independently validated by the proof checker. Use
`--no-rzone-symmetry` for the exact-orientation baseline.

`rzone_active_pruning=true` (or `--rzone-active-pruning`) lets an expanded
Environment AND node reuse a Player-Win certificate as soon as one child
produces it. The solver directly tests that specific certificate against every
remaining placement sibling instead of making each sibling scan the full
RZone index. A match becomes the same lightweight `RZONE_TT` Win leaf as a
normal lookup, in the original placement order, so the current SGF proof and
proof checker behavior are unchanged. The optimization requires
`rzone_mode=lookup`; use `--no-rzone-active-pruning` for the passive-lookup
baseline.

Placement diagnostics are printed in the final solver summary and saved in the
SGF root metadata (with a `rzone_` prefix):

- `placement_lift_blocked` is split into mutually exclusive first-failure
  reasons: `placement_lift_blocked_invalid_child`, `_mask_empty`,
  `_parent_mismatch`, `_no_placements`, `_placement_mismatch`, and `_publish`.
  These suffixes all follow `placement_lift_blocked`. Their sum equals the
  blocked total; successes plus blocked equals attempts. `_mask_empty` means
  the child mask includes any cell empty in the parent, including a newly
  placed tile's cell or a required empty cell. This check precedes parent
  pattern comparison and placement enumeration. Counts are lift attempts,
  not distinct certificates or Environment nodes.
- `environment_all_win_nodes` counts expanded Environment nodes whose children
  all resolve to Win, including siblings resolved by active pruning.
  `environment_all_win_without_rzone` counts the subset still lacking a parent
  RZone certificate after lifting. Divide the latter by the former to measure
  the fraction of completed AND proofs that cannot propagate an RZone. This
  includes cases with missing child certificates (for example, exact TT hits).
  Terminal/cache-hit Environment nodes, Loss/Unknown nodes, and runs with
  RZone disabled are excluded. Both generation and lookup modes count these
  nodes. Statistics describe the current solve, even when caches are loaded.
- Missing-parent proofs are further partitioned into
  `environment_no_rzone_all_children_rzone`,
  `environment_no_rzone_some_children_rzone`, and
  `environment_no_rzone_no_children_rzone`. Their sum equals
  `environment_all_win_without_rzone`. These count directly attached child
  certificates, including active-pruned leaves; they do not recover certificates
  through TT source references or perform extra RZone lookups.
  `environment_no_rzone_multiple_child_rzones` is a subset of the all-children
  category with more than one `(certificate ID, transform)` pair. Different
  identities do not necessarily mean different masks or logically distinct proofs.
  Divide the all-children category by missing-parent nodes for its share of the
  bottleneck, or by all-Win Environment nodes for its share of all completed AND
  proofs. These are candidates for proof composition, not guaranteed new reusable
  certificates: composition still needs a sound parent pattern and full placement
  coverage over every completion of that pattern.

Existing complete SGFs containing the original Environment diagnostic totals can
be recounted without solving again:

```bash
g++ -std=c++17 -O2 solver/tools/sgf_rzone_diagnostics.cpp -o /tmp/sgf_rzone_diagnostics
/tmp/sgf_rzone_diagnostics path/to/solve.sgf > child_rzone_coverage.json
```

The utility streams the tree in depth-sized memory, includes active-pruned leaves,
and rejects a recount that disagrees with the original all-Win/missing-parent
totals. It accepts the complete tree layout emitted by `SgfWriter`; it does not
replay moves or independently verify the proof.

This first version stores only Player-view Win proofs. Exact TT remains the
fallback for Loss results and Win states whose RZone derivation stops. RZone
certificates can be loaded, extended, and atomically saved as a `.rz` snapshot.
SGF binds the child-first record prefix used by its solve, so another opening
may append certificates without invalidating the older SGF.

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

## Multi-opening batch mode

Place ordered openings in a text file, one `tile@position,tile@position`
specification per line:

```text
2@0,2@1
2@0,2@5
2@0,4@1
```

Then solve the complete sequence with one command:

```bash
solver/solver2048 \
  --config solver/solver.cfg \
  --openings-file openings.txt \
  --output-dir result/target32-multi \
  --verify
```

For a short sequence, repeat `--opening` instead of using a file:

```bash
solver/solver2048 --config solver/solver.cfg \
  --opening 2@0,2@1 --opening 2@0,2@5 \
  --output-dir result/target32-multi
```

Batch mode requires the exact TT in its first version. It starts with empty
proof tables, keeps the accumulated TT/RZone in memory between openings, and
writes only the final `proof.tt` and optional `proof.rz`. Each opening gets an
ordered `sgf/000001.sgf` file. `proof_sequence.txt` binds those SGFs to the
final artifacts, while `batch_status.txt` records per-opening outcome, nodes,
time, and cumulative record counts. Existing managed output files are never
overwritten; resume/checkpoint support is intentionally deferred.
The single-run-only `opening` and `sgf_path` keys may be omitted from a config
used with `--openings-file` or `--opening`; TT/RZone paths and load/save flags
are also replaced by the batch-managed files below `--output-dir`.

Only a `WIN` opening is committed to the sequence. A non-Win result writes its
diagnostic SGF, stops the batch, and publishes neither final artifacts nor a
sequence manifest. `--verify` releases the Solver tables after saving and runs
the same ordered source-table verification as the standalone checker.

The generated sequence can also be checked later with:

```bash
solver/proof_checker --sequence result/target32-multi/proof_sequence.txt
```

The original no-argument/config-driven single-opening mode is unchanged.

## Proof checker

Build the first logical proof checker with:

```bash
make proof-checker
```

Verify a completed SGF/TT pair with:

```bash
solver/proof_checker \
  --sgf result/RZ/8_RZ.sgf \
  --tt result/RZ/8_RZ.tt \
  --rz result/RZ/8_RZ.rz
```

For an append/reuse chain, pass every SGF in chronological order and supply
the final TT/RZone snapshots:

```bash
solver/proof_checker \
  --sgf result/opening-001.sgf \
  --sgf result/opening-002.sgf \
  --sgf result/opening-003.sgf \
  --tt result/final.tt \
  --rz result/final.rz
```

The checker reopens all supplied files and independently validates both binary
formats, checksums, and RZone certificate semantics. It streams the entire SGF
DFS trace, independently replays every slide and placement, and verifies
terminal classification, phase alternation, AND/OR backup,
short-circuit/skipped actions, and every retained TT/RZone leaf. A node becomes
a reusable proof source only after its complete subtree and backup rule pass.
Trace-local TT leaves must reference an already completed source with the same
canonical board and phase; their Player witness comes from that verified SGF
source rather than from the TT record. Every bound TT key and RZone certificate
must have a corresponding verified SGF source. RZone binary loading still
independently checks each certificate's child-first derivation and universal
pattern semantics once. The normal checker no longer replays a second logical
proof DAG from the root. It does not load the N-tuple network or call the Solver
DFS.

New SGFs bind the exact TT snapshot by format version, CRC64, byte size, and
Player/Environment record counts. RZone binding instead uses format/semantics
versions plus a record-prefix count and checksum. A later `.rz` containing that
same prefix is valid, enabling append-only cross-opening reuse; SGF certificate
IDs cannot exceed the bound prefix. Older SGFs without RZone data continue to
verify without `--rz`. In multi-opening mode, each SGF is replayed exactly once.
Trace-local TT references may use only completed sources in the current SGF;
persistent TT/RZone references may use only sources established by earlier
SGFs. Declared loaded-entry counts must equal the accumulated earlier sources,
while the checker derives newly verified states directly from each trace. The
last SGF is bound exactly to the supplied final TT checksum and counts. Earlier
TT snapshots need not remain on disk or carry a checksum because TT
serialization is not prefix-stable. Every RZone
manifest is checked directly against the supplied final append-only prefix.
The final TT and RZone must be fully covered by the ordered SGF sequence.

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
uses `tt_source_node_id`; a persistent hit omits that run-local reference.
Node IDs are implicit DFS-preorder positions. The checker reconstructs the
phase, board, canonical TT key, transform, terminal reason, child count, and
slide reward instead of storing those values in every SGF node. RZone nodes
retain only their certificate ID and transform; the remaining certificate
data comes from the bound `.rz` file.

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

### RZone viewer

The RZone viewer uses the validated C++ `.rz` reader and a disposable JSON
export rather than duplicating the persistent binary parser in JavaScript.
Build the exporter and create a table-only view with:

```bash
make rz-viewer-export
solver/rz_viewer_export \
  --rz result/APTest/32_RZ_Pruning.rz \
  --output result/APTest/32_RZ_Pruning.viewer.json
```

Add `--sgf PATH` (repeatable) to associate certificates with their search
nodes, candidate boards, outcomes, transforms, and per-run RZone statistics.
For a multi-opening batch, use the generated sequence directly:

```bash
solver/rz_viewer_export \
  --sequence result/32_multi/proof_sequence.txt \
  --output result/32_multi/rzone_viewer.json
```

Open `solver/rz_viewer/index.html` and load or drop the JSON. The viewer shows
the complete-table kind/phase/cell distributions, an exact certificate reuse
ranking, filterable certificate records, 4x4 RZone overlays, recursive proof
chains, and SGF occurrences. With SGFs, referenced certificates, the 1,000
most-used certificates, and their child closure are exported by default so a
large `.rz` does not automatically expand into a much larger JSON file;
table-wide statistics still cover every record. Change the reuse ranking bound
with `--top-hits N`, or pass `--all-certificates` when the full table browser is
required.
For very large SGFs, `--hits-only` keeps only `solved_by=RZONE_TT`
occurrences and their proof chains while preserving the full-table statistics.
Occurrence details are deterministically reservoir-sampled to 50,000 records
by default. The exporter still scans the complete SGF and stores exact global
and per-certificate `solved_by=RZONE_TT` hit counts, so sampling does not affect
the **Top reused** ranking. Ordinary lookup hits and active-pruning hits share
that SGF marker and are therefore combined. Use `--max-occurrences N` to
change the browser payload limit, or `0` to explicitly request an unbounded
export.

### Scalable indexed search-tree viewer

`solver/tree_viewer` keeps the complete SGF search tree logically expanded but
loads and paints only the current DFS-order/depth viewport. It avoids loading a
multi-GB SGF into JavaScript or creating one SVG element per node. Build an
atomic fixed-record index and start its local backend with:

```bash
make tree-viewer-index
solver/tree_viewer_index \
  --sgf result/APTest/32_RZ_Pruning.sgf \
  --rz result/APTest/32_RZ_Pruning.rz \
  --tt result/APTest/32_RZ_Pruning.tt \
  --output result/APTest/32_RZ_Pruning.tree-index
python3 solver/tree_viewer/server.py serve \
  --index result/APTest/32_RZ_Pruning.tree-index \
  --port 8080
```

Open `http://127.0.0.1:8080/`. The Canvas view starts with the complete DFS
trajectory, switches between aggregate/sampled and exact nodes as it zooms,
and requires no branch-expansion clicks. Selecting a node retrieves its board,
RZone overlay and proof chain, root path, and same-SGF TT source. See
`solver/tree_viewer/README.md` for the index format, performance measurements,
and current persistent-TT/composite-RZone limitations.

## Tests

```bash
make solver-test
```

The suite includes the complete unlimited target-8 search with TT enabled. The
far larger unlimited raw DFS baseline (`--no-tt`) remains an opt-in benchmark.
