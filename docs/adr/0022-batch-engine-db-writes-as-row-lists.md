---
status: accepted
date: 2026-10-12
---

# Batch engine-db writes as row lists: one statement per table, nodes before relationships

## Context and Problem Statement

Loading the playground's `convergence` score (800 partials, 20 frames) took
about 53 s in server mode and 62 s end to end in batch mode, on the fully
optimised `macos-max` build. Almost all of that was precomputation writing
its output (`rendering-engine/docs/precomputation.md` §10): about 30,000
statements, of which 16,000 created one `FrameEntry` each.

Each partial was already written in one transaction, so the cost was not the
commits. It was executing the statements. A prepared statement keeps its
logical plan, but every execution still binds its parameters, builds a
physical plan, schedules a pipeline, and runs whatever joins a
`MATCH … WHERE x.id = $p` needs. One node or one relationship per statement
spends nearly all its time on that overhead.

How should `rendering-engine` write many rows to `engine-db` so that the
per-statement cost is paid per table, not per row?

## Decision Drivers

* **Throughput while on Ladybug.** The graph model is kept because writing
  the system down as a graph forces rigour and clarity. It may later give way
  to custom C++ structures, but while Ladybug is the store, which may be for
  good, it has to load scores at a reasonable rate.
* **The model is unchanged.** No schema change and no denormalisation.
* **Per-partial atomicity** (precomputation.md §9). A partial's output is
  either wholly present or wholly absent, and one partial's failure cannot
  affect another's.
* **No full table scans per row.** Lookups go through a primary-key index,
  as checked with `PROFILE` (the method used for ADR 0011).
* **Ladybug types stay inside `engine_db.cpp`.**

## Considered Options

1. **Commit grouping only**: keep one statement per row, and commit many
   partials per transaction.
2. **Row lists**: bind a table's rows as one `LIST(STRUCT)` parameter. Nodes
   go in by `UNWIND $rows AS r CREATE … RETURN r.k, n.id`. Relationships go
   in by `UNWIND … MATCH (a {id: r.from}), (b {id: r.to}) CREATE …`.
3. **Row lists, with `COPY` for relationships**: as option 2 for nodes, but
   relationships by `COPY R FROM (UNWIND $rows AS r RETURN r.from, r.to)
   (from='A', to='B')`.
4. **`COPY` for nodes as well.**
5. **Scanning an in-memory table (Arrow, a data frame) into `COPY`.**

## Decision Outcome

Chosen option: **3, row lists with `COPY` for relationships**, implemented as
a new `WriteBatch` layer over `EngineDb`. It is the only option that makes
both nodes and relationships cheap per row while keeping index lookups.
Measured on Ladybug v0.20.4-rainbohz.2 at `-O3` with ThinLTO, with 100,000
nodes already in the table:

| Write | Cost per row |
| --- | --- |
| Node, one per prepared statement | 129 µs |
| Node, `UNWIND $rows CREATE … RETURN`, ≥ 100 rows per statement | 18 µs |
| Relationship, one per prepared statement by `MATCH` | 373 µs |
| Relationship, `UNWIND … MATCH … CREATE`, 10 to 10,000 rows per statement | 1,500 to 27,000 µs |
| Relationship, `COPY … FROM (UNWIND $rows …)`, ≥ 1,000 rows per statement | 15 µs, flat as the table fills from 0 to 90,000 relationships |

`PROFILE` shows `COPY` finding both ends by `INDEX_LOOKUP`. The batched
`MATCH` falls into a join over scans, so it is far worse than one row at a
time. Option 2 is therefore refused for relationships.

`COPY` was also established, by experiment and by the tests in
`test_engine_db.cpp` and `test_write_batch.cpp`, to:

* bind `$rows` from C++ inside a subquery;
* run inside an explicit transaction;
* see nodes inserted earlier in the same, still uncommitted transaction.

**How the layer works** (`write_batch.hpp`):

* An operation is registered once per table, or per FROM/TO pair of a
  relationship table, and rows are staged against it.
* A new node is referred to by a `NodeHandle` until its id is known.
* `flush()` runs in one transaction, in a fixed order:
  1. delete statements;
  2. node statements and node inserts, which report each new node's id by
     handle;
  3. relationship inserts, with handles resolved to ids.

  Identical rows of a create-if-missing (`MERGE`) statement are sent once.
* Rows are grouped in **units**. If the batched transaction fails, it is
  rolled back and every unit is retried in a transaction of its own. Each
  unit is then written whole or not at all, and one bad unit costs only
  itself. Precomputation uses one unit per partial and flushes every 64
  partials.

**Why two phases of writes are always enough.** Node properties never hold
another node's id. Every reference between rows is a relationship, which in
relational terms is a junction table of `(from_id, to_id)` rows, not a
foreign key. Entity tables can therefore all be loaded before any junction
table, whatever the mix of tables. This is a rule of the schema, and the
layer depends on it: **no node property may hold another node's id.**

### Consequences

* Good, because loading and rendering `convergence` in batch mode fell from
  62.3 s to 24.2 s on `macos-max`, and precomputation within it from about
  40 s to 2.4 s. Every example score's audio is byte-identical, and so is
  every table's row count. What remains is loading the score's Cypher
  (about 5.5 s), the transform, which still writes one row per statement
  (about 3.7 s), and synthesis (about 5.9 s for 20 frames).
* Good, because the statement count per flush depends on the number of
  tables and pairs (28 operations for precomputation), not on the number of
  rows.
* Good, because every delete goes by id. Precomputation now reads an earlier
  run's `FrameEntry` and nominal-frequency ids in its bulk read, rather than
  finding them through a hop per partial.
* Good, because computing output and writing it are now separate steps.
  That suits any of the parallel-precomputation options still under
  consideration.
* Bad, because a write failure is no longer seen in the order the partials
  were processed. It is attributed by the retry, which costs one transaction
  per unit, but only when something fails.
* Bad, because a failed write is now reported as Ladybug's error text. The
  specific message "the partial disappeared while it was being precomputed"
  is gone; a partial cannot disappear during a run on the engine's one
  connection.
* Neutral, because `SERIAL` ids are assigned by a sequence and their order
  follows rows within a statement, not partials. Nothing may assume an order
  among them. The handle map is the only link between a staged node and its
  id.
* Neutral, because deleting a partial's `PARTIAL_IN_FRAME` edges on a
  rebuild still goes through a hop from the partial. It runs once per flush,
  and only for partials that had scope before. A first load does not run it.

Impact on components:

* `rendering-engine` — `EngineDb` binds a `DbRecordList` parameter as
  `LIST(STRUCT)`. The new `WriteBatch` stages and flushes rows.
  Precomputation stages each partial as one unit. The transform's writes and
  rendering's per-frame statements are not yet changed, and are the next
  candidates.
* `engine-db` — no schema change. The rule above, no node property holding
  another node's id, now matters to the engine as well as to the model.

### Confirmation

* `test_write_batch.cpp` covers:
  * lists whose nodes are new in the same flush;
  * a distinct create-if-missing statement;
  * deletes running before inserts, under a `ONE_ONE` relationship;
  * a failing unit leaving the others written and itself absent;
  * misuse refused.
* `test_engine_db.cpp` covers the record-list binding and `COPY` from it.
* The precomputation whole-run tests pass unchanged: the worked example,
  idempotent rebuild, replaced compensation segments, a failing partial
  among good ones, a failed rebuild keeping the earlier output, and lists
  over 30 points. So do the rendering oracle and `make test-hzl-oracle`.
* Nothing checks the schema rule automatically. A node property that held
  an id would still load, and would only break if a batch had to resolve it.

## Pros and Cons of the Options

### 1. Commit grouping only

* Good, because it is a two-line change.
* Bad, because it saves almost nothing: partials were already one
  transaction each, and the cost is per statement, not per commit.

### 2. Row lists, relationships by `UNWIND … MATCH`

* Good, because one statement form serves nodes and relationships alike.
* Bad, because the relationship plan joins over scans. It is 4 to 70 times
  slower per row than one statement per row, and worse as tables grow.

### 3. Row lists, relationships by `COPY`

* Good, because both kinds of write cost about 15 to 18 µs per row, and
  `COPY` uses the primary-key index.
* Good, because it needs only the public C++ API and a parameter type.
* Bad, because `COPY` cannot return what it wrote. That is harmless for
  relationships, which carry no properties in the output tables.

### 4. `COPY` for nodes as well

* Good, because it is the documented fastest bulk path.
* Bad, because `COPY` skips `SERIAL` columns and does not report the ids it
  assigns. They come from a sequence, so they could only be guessed by
  assuming they are contiguous. The relationships that follow need them.

### 5. Scan an in-memory table into `COPY`

* Good, because it would avoid building a `Value` per field.
* Bad, because Ladybug offers it only to Python: a parameter must be a
  pointer to a registered scan replacement. The C++ hook
  (`ClientContext::addScanReplace`) is internal API.

## More Information

* Code: `rendering-engine/include/rainbohz/rendering_engine/write_batch.hpp`,
  `rendering-engine/src/write_batch.cpp`, `EngineDb::query` (`DbRecordList`),
  and `stagePartial` in `rendering-engine/src/precomputation.cpp`.
* Design: `rendering-engine/docs/precomputation.md` §6.1, §6.2, §9 and §10.
* Ladybug sources consulted: `binder/bind/copy/bind_copy_from.cpp` (COPY
  binding; `SERIAL` skipped for nodes, read as `INT64` for relationship
  ends), `binder/bind/bind_file_scan.cpp` (query scan sources),
  `binder/bind/bind_ddl.cpp` (`SERIAL` as `nextval`), and
  `main/client_context.cpp` (`executeWithParams` reusing the cached plan).
* Revisit if Ladybug's planner learns to use index lookups for
  `UNWIND … MATCH` on keys. Option 2 would then serve relationships
  carrying properties, which `COPY` can also carry but which none of the
  output tables has today.
