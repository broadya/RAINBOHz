---
status: accepted
date: 2026-10-02
---

# The lowest HzL level is the canonical model as tables, emitted as Cypher

## Context and Problem Statement

`architecture/overview.md` §6 says `hzl-compiler` lowers HzL to its lowest
abstraction level and emits that as `canonical-form`, which `rendering-engine`
loads into `engine-db`. It left open what the lowest level and `canonical-form`
actually are (§10, "Loading and compilation"). The first version of the
compiler had to choose, because it has to emit something the engine can load
today, without closing off the loading path a score at target scale will need.

## Decision Drivers

* The engine already loads Cypher in process, statement by statement
  (ADR 0005), and `engine-db/examples/` are hand-written Cypher scores. An
  output the engine loads unchanged means the compiler can be proved against
  those scores now, with the existing oracle.
* A score at target scale is 1M+ partials. One Cypher statement is not going
  to carry that, and Ladybug has a bulk path, `COPY FROM` CSV or Parquet, that
  reads straight into a table.
* "Cypher as the database interface" is Settled in `overview.md` §9, and
  `engine-db`'s schema is the source of truth for the model. The lowest level
  should add nothing the schema does not have.

## Considered Options

* **Cypher is the lowest level**: the compiler's final data structure is the
  text of the statements.
* **The canonical model as tables is the lowest level**: one row list per node
  table and one edge list, shaped as `engine-db` is; Cypher is one
  serialisation of it.
* **A compiler-owned intermediate format** (a YAML or JSON "canonical-form"
  document with its own structure), translated to Cypher by a separate step.

## Decision Outcome

Chosen option: "the canonical model as tables", because it is what every
loading path wants. Cypher `CREATE` patterns are a walk over the rows; a
columnar file for `COPY FROM` is another walk over the same rows. Choosing
Cypher itself as the level would mean parsing it back to get the second path;
choosing a compiler-owned format would mean inventing a second model of the
same data beside the schema.

The rows carry only what the schema declares, with compiler-assigned local
keys. The Cypher emitted today is one `EngineConfig` statement, then one
statement per table in chunks of 1,000 rows: `UNWIND [rows] CREATE` for
nodes, and `COPY ... FROM (UNWIND [rows] ...)` for relationships, which name
their ends by id. The ids are the row indices, because every score loads
into a fresh database. `COPY` finds both ends by primary-key index, so
loading does no scan. Until 2026-10-13 the score was a single `CREATE`
clause, whose binding cost grows faster than the square of its size: 7 s of
binding at 800 partials, 72 s at 1,600 (`hzl-compiler/docs/design.md` §7).

### Consequences

* Good, because the compiled route is proved against the hand-written route
  by the existing oracle, with no engine or schema change
  (`make test-hzl-oracle`).
* Good, because a columnar output for `COPY FROM`, when parsing literal rows
  becomes the cost, is a second emitter over the same tables, not a
  redesign.
* Neutral, because what is loaded is for now a `.cypher` file. `canonical-form`
  is the lowest level — the tables — and is not loaded directly; the file is
  its first serialisation. `overview.md` §10 still lists the serialisation at
  target scale as open (Parquet is a strong candidate); this decides the first
  version's answer, not the final one.
* Bad, because literal rows cost about 55 µs each to parse and bind, which
  is fine for thousands of partials but is many minutes at the target scale
  (1M+), where the columnar emitter is needed.
* Bad, because relationship ends are ids predicted by the compiler, which
  holds only for a load into an empty database.

Impact on components:

* `hzl-compiler` — its final data structure is one row list per node table
  and one edge list (`canonical/model.py`). `canonical/cypher.py` is one
  emitter over it, and a `COPY FROM` emitter is a second.
* `canonical-form` — the lowest HzL level, the canonical model as tables. Its
  serialisation for loading is for now a `.cypher` file: one `EngineConfig`
  statement, then batched `UNWIND ... CREATE` and `COPY ... FROM (UNWIND ...)`
  statements, table by table, with no `MATCH`.
* `rendering-engine` — loads a compiled score unchanged through `--state`,
  with no index lookups and no scans. At target scale it will need a bulk
  load path for the columnar output.
* `engine-db` — its schema stays the only model of the data. A schema change
  has to be followed by the compiler's rows, or the compiled examples fail to
  load.

### Confirmation

`make test-hzl-compiler` compares the emitter's output for the tempo-map
example with a golden file; `make test-hzl-oracle` renders the compiled score
and checks it against the hand-written score's oracle. Nothing enforces that
the row model stays the schema's shape; a schema change that the compiler
does not follow shows up as a load failure in those tests.

## More Information

`hzl-compiler/docs/design.md` §6 and §7; `canonical/model.py` and
`canonical/cypher.py`. To be revisited when parsing literal rows becomes the
cost of a load, which is the trigger for the `COPY FROM` emitter, or when
`overview.md` settles how `canonical-form` is serialised for loading.

ADR 0005 for how the engine loads the emitted Cypher, and
`rendering-engine/docs/transform.md` for what the engine does with the
canonical tables once they are loaded.
