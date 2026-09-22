# Indexed Search Tree Viewer

This viewer keeps the complete search tree logically expanded while loading
only the current DFS-order/depth viewport. It is intended for SGFs that are too
large for `File.text()`, JavaScript object trees, or a complete SVG layout.

The first indexing pass streams the SGF and writes one 64-byte random-access
record per search node. Boards are replayed once and stored as their 64-bit
encoding. RZone and TT snapshot paths remain external and are opened by the
local server. The UI requests 9,000 node records per viewport (the API caps
requests at 20,000); wide views use deterministic sampling plus exact overview
buckets.

## Build an index

Build the optimized streaming indexer:

```bash
make tree-viewer-index
```

For the target-32 active-pruning result:

```bash
solver/tree_viewer_index \
  --sgf result/APTest/32_RZ_Pruning.sgf \
  --rz result/APTest/32_RZ_Pruning.rz \
  --tt result/APTest/32_RZ_Pruning.tt \
  --output result/APTest/32_RZ_Pruning.tree-index
```

The Python backend also has an `index` subcommand used as a portable reference
implementation and by unit tests. Use the optimized C++ command above for real
solver results.

Index construction is atomic: the requested output directory appears only
after the complete SGF has been parsed. An existing output directory is never
overwritten. At 64 bytes per SGF tree node, 85.7 million nodes require about
5.1 GiB for `nodes.bin`, plus a small overview and metadata file. SGF tree
nodes include visited nodes and active-pruned proof leaves. The root statistics
retain the separate `player_nodes` and `environment_nodes` visited-node counts.

## Run the local viewer

```bash
python3 solver/tree_viewer/server.py serve \
  --index result/APTest/32_RZ_Pruning.tree-index \
  --port 8080
```

Open <http://127.0.0.1:8080/>. The browser does not need direct filesystem
permission for the SGF, RZ, TT, or index files.

The initial view frames the beginning of the first root-to-leaf DFS trajectory
at an interactive node size. **Root trajectory** returns to that local view,
while **Fit complete tree** shows the global search overview. Click a node to
select it, drag the canvas to pan, and use the wheel to zoom. The arrow keys
navigate structurally: Up selects the
parent, Down the first child, and Left/Right the previous/next sibling; Home
returns to the root. Keyboard navigation preserves the current camera while
the selected node remains visible and only pans enough to reveal a node that
crosses the viewport edge. At overview scale, bands show exact per-range Terminal,
RZone, and TT densities. At detail scale, game-solver-style nodes show their
AND/OR role and incoming action. Selecting a node loads its board, RZone
overlay and proof chain, action path, and same-SGF TT source without expanding
any branch. When a detail node's parent is outside the viewport, its incoming
edge is clipped to a continuation marker on the viewport boundary instead of
disappearing.

On the current target-16 active-pruning SGF, the C++ indexer processed 413,631
SGF tree nodes in about 1.7 seconds and produced a 26 MiB node index. A complete
overview API request returned 9,045 sampled/special nodes in about 0.1 seconds;
an 8,001-node exact viewport returned in about 0.08 seconds on the development
server.

The full target-32 active-pruning SGF was also exercised end to end: 85,718,559
SGF tree nodes, maximum depth 127, and a 5,485,987,776-byte node index were
produced in 5 minutes 44 seconds. The complete-tree overview returned 10,411
sampled/special nodes plus 476 aggregate bins (3.1 MB JSON) in about 0.17
seconds. An exact 8,001-node viewport returned in about 0.08 seconds.

## Current first-version boundaries

- The spatial layout is DFS visit order on X and search depth on Y. A tidy
  node-link layout can be added later without changing the node index.
- RZone v1 fixed records are supported. Composite-placement proof DAG records
  will require the newer variable-record snapshot index.
- Same-SGF TT source links work through `tt_source_node_id`. Persistent TT
  entries can be identified, but their originating opening is not present in
  the current SGF/TT formats.
- Ordinary RZone lookup and active-pruning matches both use `RZONE_TT` in the
  current SGF, so they are not separately colored.

## Tests

```bash
python3 -m unittest solver/tree_viewer/test_server.py
```
