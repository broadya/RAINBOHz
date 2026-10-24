---
status: accepted
date: 2026-10-21
---

# Compile scores to Parquet tables with a manifest, and keep Cypher as the debugging form

## Context and Problem Statement

`hzlc` wrote a score as one Cypher script: one statement per table, with
every row written as a literal (`hzl-compiler/docs/design.md` §7). The engine
handed each statement to Ladybug as text. On `echoes`, a generated piece of
5,120 partials and 82,005 rows, loading that script took 9.2 s of a 27 s
load-and-render on `macos-max`. Sampled, 49% of that was Ladybug's ANTLR
parser reading the literals, 36% the binder typing them, and 14% storing the
rows. The cost grows with the number of rows, at about 112 µs each. How
should a compiled score reach `engine-db` so that loading it costs little
more than storing it?

## Decision Drivers

* The load is paid on every reload of a live piece, so it bounds how quickly
  a practitioner hears a change.
* Relationships name their ends by id, and the ids must be the compiler's row
  indices (`hzl-compiler/docs/design.md` §7, "Ids"). Any route must keep that
  exactly, or relationships connect the wrong nodes without any error.
* A readable form that can be loaded by hand is worth keeping for debugging.
* In server mode, a score must not reach files outside `--input-root`.
* The audio must not change: both forms must give byte-identical frames.

## Considered Options

* Parquet files with a JSON manifest; the engine writes the `COPY FROM`
  statements
* CSV files with a JSON manifest
* CSV or Parquet files plus a Cypher script of `COPY FROM` statements
* Rows as JSON, written by the engine through `$rows` parameters, as
  `WriteBatch` does

## Decision Outcome

Chosen option: "Parquet files with a JSON manifest; the engine writes the
`COPY FROM` statements", because Ladybug then reads the rows as data with
nothing parsed, the files are typed, and the engine alone decides which files
are read and how.

`hzlc compile <score> -o <dir>` writes `<dir>/manifest.json` and one Parquet
file per node table (`<Table>.parquet`) and per relationship (FROM, TO) pair
(`<REL>.<From>.<To>.parquet`). A node table with no columns has no file:
the manifest gives its row count. The manifest (`format`
`"rainbohz-score-tables"`, `version` 1) carries the `EngineConfig`, the node
tables in load order with their columns and row counts, and the relationship
pairs. `hzlc --debug-cypher -o <file>.cypher` writes the Cypher script as
before. It is the debugging form.

`rendering-engine --state` and `PUT /v1/score` take either form and choose by
extension: `.json` is a manifest, `.cypher` a script. For a manifest the
engine builds every statement itself: `COPY <Table>(<columns>) FROM
'<file>'` for each node table, `UNWIND range(1, n) … CREATE` for a table
with no columns, then `COPY <REL> FROM '<file>' (from=…, to=…)` for each
relationship pair.

**One row group per node file.** Ladybug reads a Parquet file one row group
per task, in parallel, and numbers the rows in the order the tasks finish. A
node file of several row groups therefore gets ids out of file order. This
was seen on `echoes`, where frames differed by more than their own peak.
`parallel=false` is refused for Parquet. `hzlc` writes every node file as one
row group whatever its size, because pyarrow's default splits a table at
1,048,576 rows. Relationship files may be split, since edge order does not
reach the audio.

Measured on `echoes` (`macos-max`): the score loads in about 0.11 s from
tables against 9.2 s from Cypher, and every frame is byte-identical.

### Consequences

* Good, because loading a score costs about 1.3 µs a row instead of about
  112 µs, an 80-fold drop on `echoes`. The transform, precomputation and
  rendering are now what a reload waits for.
* Good, because the manifest is checked before anything is executed. Names
  must be identifiers, and every file must be a plain `.parquet` name that
  resolves, symlinks followed, inside the manifest's own directory. Since the
  manifest is under `--input-root`, so is every file the engine reads.
* Good, because the engine checks that each node table is empty before its
  rows go in, and that it holds the manifest's count afterwards. The
  fresh-database assumption behind the ids is enforced, not merely stated.
* Good, because the Cypher form stays loadable everywhere a score is, and the
  hand-written example states are Cypher and load unchanged.
* Bad, because `hzl-compiler` gains a dependency on pyarrow, which is large
  (about 40 MB).
* Bad, because the compiled score is a directory of binary files rather than
  one text file. Reading it needs a Parquet reader, or `--debug-cypher`.
* Neutral, because hzlc's normal output path changed from a `.cypher` file to
  a directory. hzlc refuses `-o x.cypher` without `--debug-cypher`, rather
  than quietly making a directory of that name. Callers outside the
  repository have to change.
* Neutral, because each node table is read by one task. Loading many tables
  at once, or splitting a table across tasks, would need ids that do not
  depend on read order.

Impact on components:

* `hzl-compiler` — `canonical/tables.py` writes the tables and the manifest.
  `--debug-cypher` selects `canonical/cypher.py`, which is unchanged apart
  from sharing the grouping of relationships.
* `rendering-engine` — `table_loader.{hpp,cpp}`: `loadTableManifest()` and
  `loadStateFile()`, used by the score pipeline in both modes. A manifest
  fault is a `ManifestError`, answered 422 by `PUT /v1/score`.
* `engine-db` — no schema change. The `SERIAL` ids count from 0 in the order
  rows are copied in, as for the Cypher form.
* `playback-client` — none. The playground's `pg` loads tables.

### Confirmation

* `make test-score-forms` compiles every example score both ways, renders
  the same request from each, and fails unless every frame file is
  byte-identical. It is part of `make test`.
* `make test-hzl-oracle` checks the tempo-map and polytempo twins compiled to
  tables against their hand-written scores' oracles.
* `hzl-compiler/tests/test_tables.py` checks a node file past pyarrow's
  default row-group size is still one row group, and that values (NULLs,
  floats, strings) round-trip exactly.
* `rendering-engine/test/test_table_loader.cpp` checks that ids follow file
  order and that a table already holding rows is refused. It also checks
  that every malformed or escaping manifest is refused before anything runs.
  `test_server.cpp` covers loading by `PUT /v1/score`.

## Pros and Cons of the Options

### Parquet files with a JSON manifest; the engine writes the statements

* Good, because the values are typed and stored in binary, so a float is
  exact without any formatting rule, and NULL is native.
* Good, because the files are smaller than CSV (512 KiB against 1.4 MiB on
  `echoes`).
* Good, because the engine controls every statement and every path.
* Bad, because of the pyarrow dependency and the row-group rule above.

### CSV files with a JSON manifest

* Good, because it needs only the Python standard library, and the files can
  be read by eye.
* Good, because `parallel=false` keeps CSV ids in file order. Measured: 0.14 s
  on `echoes`, byte-identical.
* Bad, because every value is text again: floats need `repr()` round-trips
  and NULL is an empty field.

### Data files plus a Cypher script of `COPY FROM` statements

* Good, because the script still runs by hand.
* Bad, because Ladybug resolves a relative path against the process's working
  directory. The engine would have to rewrite paths in a script, and keep
  them inside `--input-root`, rather than build them.

### Rows as JSON, through `$rows` parameters

* Good, because it reuses `WriteBatch`'s parameter route.
* Bad, because that route still costs about 15 to 18 µs a row (ADR 0022). On
  every worker thread it also copies the whole row list, which was seen on
  2026-10-21 to cost more than the inserts.
* Bad, because the engine would have to know every table's columns from
  somewhere other than the data.

## More Information

* The measurements and profiles behind this ADR were taken on 2026-10-21
  with `macos-max`, the music-hackspace `echoes` piece and `sample` at 1 ms.
* ADR 0022, whose route for the engine's own writes is unchanged, and which
  named the Cypher literals as the remaining load cost.
* `hzl-compiler/docs/design.md` §7; `rendering-engine/docs/design.md` §3.1;
  `rendering-engine/docs/server.md` §3.1.
* Revisit if the ids stop depending on read order, which would let Ladybug
  load tables in parallel.
