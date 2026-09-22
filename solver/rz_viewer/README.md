# Solving2048 RZone Viewer

The viewer consumes JSON produced by the validated C++ `.rz` reader. It does
not parse the persistent binary format in JavaScript.

Build the exporter:

```bash
make -f makefile rz-viewer-export
```

Export one RZone table:

```bash
solver/rz_viewer_export \
  --rz result/APTest/32_RZ_Pruning.rz \
  --output result/APTest/32_RZ_Pruning.viewer.json
```

Attach one or more search SGFs:

```bash
solver/rz_viewer_export \
  --rz result/APTest/32_RZ_Pruning.rz \
  --sgf result/APTest/32_RZ_Pruning.sgf \
  --output result/APTest/32_RZ_Pruning.viewer.json
```

For a multi-opening batch, the sequence manifest supplies both `.rz` and all
ordered SGFs:

```bash
solver/rz_viewer_export \
  --sequence result/32_multi/proof_sequence.txt \
  --output result/32_multi/rzone_viewer.json
```

Open `index.html` directly in a browser and load or drop the JSON. No web
server or CDN is required.

With SGFs, the default export contains only referenced certificates and their
recursive child chains, while table-wide distributions still cover the whole
`.rz`. It also exports the most frequently reused certificates and their proof
chains (1,000 by default), so the viewer's **Top reused** view remains useful
even when occurrence details are sampled. Change that bound with
`--top-hits N`. Use `--all-certificates` to include the complete table. Without
SGFs the complete table is exported automatically.

For very large SGFs, add `--hits-only` to retain only nodes resolved by an
RZone lookup. Full-table statistics remain unchanged, and every retained hit's
recursive proof chain is still exported.

SGF occurrences are deterministically reservoir-sampled to 50,000 records by
default. The exporter still scans and validates the complete SGF and reports
the exact annotation count and exact per-certificate RZone TT hit counts; only
the occurrence detail rows are sampled. A hit is an SGF node whose
`solved_by=RZONE_TT`. The current SGF format does not distinguish ordinary
lookup hits from active-pruning hits, so the viewer reports their combined
reuse count. Change the occurrence bound with `--max-occurrences N`.
`--max-occurrences 0` explicitly requests an unbounded occurrence export and
is normally unsuitable for large target-32 traces.
The browser rejects JSON files above 256 MiB with an explicit size message
instead of surfacing Chromium's misleading `NotReadableError`.
