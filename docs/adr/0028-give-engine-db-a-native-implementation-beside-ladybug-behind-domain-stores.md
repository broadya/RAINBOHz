---
status: proposed
date: 2026-10-25
---

# Give `engine-db` a native C++ implementation beside Ladybug, behind domain stores

## Context and Problem Statement

`engine-db` is a Ladybug graph database. The model expressed there has
paid off: writing it as a graph schema forces the whole thing to fit
together. As a store on the hot path, though, it is now most of what a
practitioner waits for:

* On `echoes` (5,120 partials, 2026-10-21, `macos-max`), about 18 s of a
  31 s run was Ladybug reading and writing. Precomputation's own maths was
  28 ms; nearly all of transform and precomputation's 7.4 s was WriteBatch
  flushes ([ADR 0022](0022-batch-engine-db-writes-as-row-lists.md)).
* After threads ([ADR 0026](0026-render-a-frames-partial-sets-on-worker-threads-bit-identical-to-serial.md))
  and the SIMD kernel ([ADR 0027](0027-render-chunks-with-a-highway-simd-kernel-checked-against-the-scalar-reference.md)),
  the serial frame gather dominates each rendered frame.
* A batched hop along `NEXT_POINT` (`UNWIND $ids … MATCH (a {id: id})-[]->(b)`)
  scans the whole node and relationship tables, in 0.20.4 and in 0.21.2
  alike (benchmark of 2026-10-24). An upgrade does not fix it.
* Ladybug has a single writer, so transform and precomputation cannot run
  per partial on worker threads.

Ladybug still brings things worth keeping: a standard query language, a
persistent and inspectable store (`lbug`), the integrity checks
(`--validate-db`, all Cypher), and the debugging Cypher score form
([ADR 0025](0025-compile-scores-to-parquet-tables-keep-cypher-for-debugging.md)).

How can the engine gain a fast in-memory model without giving up Ladybug,
and without the graph schema stopping being the contract?

## Decision Drivers

* **The schema is the logical contract.** `engine-db/schema/engine-schema.cypher`
  is hand-edited by the author and is the data model. Nothing may become a
  second, independent definition of it.
* **Speed where the time goes:** loading, the transform and precomputation
  writes, and the frame gather.
* **Keep Ladybug selectable,** for inspection, validation, Cypher scores and
  as a reference.
* **Checkable equivalence.** Two implementations are only trustworthy if a
  test proves that they give the same output.
* **Parallel precomputation** must become possible later, without another
  restructuring.

## Considered Options

For the seam between the engine's stages and the store:

1. **Domain stores:** one typed interface per stage, which reads that stage's
   inputs and writes its outputs as domain records. Each backend implements it.
2. **A generic graph API** (node tables, relationship tables, rows), with
   the stage code unchanged.
3. **Native only for derived data,** with the canonical model staying in
   Ladybug.

For the native types:

1. **Hand-written, shaped for the stages,** with a checked mapping to the
   schema.
2. **Generated from the DDL** at build time.

## Decision Outcome

Chosen: **domain stores, with hand-written native types whose mapping to the
schema is tagged and checked; native mode does not use Ladybug at all; native
is the default for every score it can load.**

* **One database per run, two implementations.** A run uses exactly one
  `engine-db`, either the Ladybug or the native implementation, chosen by
  `--backend native|ladybug`. There is still no second database and no
  transfer between two stores (architecture/overview.md §5.2).
* **Seam.** Every stage already reads its inputs into memory, computes, and
  writes its results in one batch. That shape becomes the interface: a store
  per consumer (transform, precomputation, frame gather, score queries)
  speaking in domain records with unit types. The Cypher moves out of the
  stages into the Ladybug implementation. WriteBatch's guarantees become the
  interface's contract: units all-or-nothing, a failed unit costing only
  itself, deletes before nodes before relationships.
* **Native types are hand-written,** with the schema as their basis. Where
  the stages only walk a relationship one way, the native form encodes it
  structurally: a `NEXT_POINT` list is vector order, ownership is an index
  range. Every type, field and relationship pair carries an `@enginedb{…}`
  tag naming what it represents in the schema, and how. A table not used yet
  is listed as unmapped, with a reason. A checker run by `make test` fails
  when the schema and the tags disagree, in either direction. It reads the
  schema and never edits it.
* **Native mode never opens Ladybug.** It reads hzlc's Parquet tables and
  manifest itself, through Apache Arrow C++ built from pinned source with only
  Parquet and Snappy enabled. A Cypher score needs `--backend ladybug`.
* **No integrity checks in native mode.** Every score arrives through the
  compiler, which is responsible for its integrity. `--validate-db` is a
  software test, and runs on the Ladybug backend. With `--backend native` it
  is refused, saying why. This may be revisited.
* **Native is the default for every score it can load.** Without
  `--backend`, the score decides: hzlc's tables (a manifest) load natively;
  a `.cypher` score, which only Ladybug reads, `--validate-db` and
  `--db-path` choose Ladybug. In server mode the choice is made per
  `PUT /v1/score`. So everything the compiler emits, and so the deployed
  path, runs natively, and every Cypher workflow (the hand-written examples
  the engine's tests load, the debugging form, the integrity checks) keeps
  working without a flag. An explicit `--backend native` refuses a Cypher
  score, `--validate-db` and `--db-path`, naming `--backend ladybug`.
* **Orders are stated, not inherited.** Where a stage depends on the order
  of the rows a read returns, the order is written out in full, in the Cypher
  (ORDER BY, with `x IS NULL` before `x` where a NULL may occur) and in the
  native read alike. It is never left to Ladybug's plan, which the native
  side could not reproduce. The equivalence test found one such read, a
  tempo map's synchronisation edges, and it is now stated on both sides.

Generated types were not chosen. They would mirror Ladybug's layout,
junction tables and linked lists included, and that shape is much of the
cost. They also could not hold unit types without teaching the generator the
mapping. The checked tags give back most of generation's protection against
drift.

A generic graph API was not chosen: the native side would become a
hand-built graph database with a query layer, and lose most of the gain.

Native for derived data only was not chosen: loading and the frame gather
would still go through Ladybug, and they are two of the three costs.

### Consequences

* Good, because loading, writing and gathering become columnar reads, vector
  appends and index lookups, linear in the rows they touch.
* Good, because the stages no longer contain Cypher. Their database code is
  one interface, and parallel transform and precomputation become a change to
  the native store and the stages, not to the architecture.
* Good, because the schema stays the contract: a schema edit fails
  `make test` until the native side has been looked at.
* Bad, because there are two implementations of every read and write, and
  each schema change has native work to go with it.
* Bad, because results the stages read in no stated order (a Cypher
  query without ORDER BY) must still come out the same from both backends.
  Where a stage depends on such an order, the fix is to state it, in the
  query and in the native store alike.
* Bad, because the checker verifies names and coverage, not meaning. A field
  with the wrong unit passes it. Only the cross-backend oracle catches that,
  and only for what the example scores exercise.
* Bad, because Arrow is a large dependency with a long first build.
* Neutral, because native mode has no persistence. The Parquet score is the
  durable form, and the server's `PUT /v1/score` replaces the whole model, as
  it does today.

Impact on components:

* `rendering-engine` — stages reach `engine-db` only through domain stores;
  `--backend` selects the implementation, and without it the score does
  (native for tables); `--validate-db` and Cypher scores require
  `ladybug`.
* `engine-db` — two implementations of one schema. The `.cypher` schema stays
  the single definition, hand-edited by the author.
* `hzl-compiler` — none. Its Parquet tables are what native mode reads.
* `playback-client` — none. Frames arrive sooner.
* `playback-server` (not built yet) — none. Each shard is still one
  `rendering-engine` with one `engine-db`.

### Confirmation

* `make check-engine-db-mapping`, part of `make test`, fails on any table,
  property or relationship pair that is neither tagged nor listed as
  unmapped, and on any tag that names something the schema lacks.
* `make test-score-forms` renders every example score natively and on
  Ladybug (from tables and from Cypher), with the same `--render-kernel`, and
  requires every frame file and every report to be byte-identical.
* Each stage has a test that runs it on both stores from the same compiled
  score and requires equal records: the transform's and precomputation's
  units, the gathered frames and the timing answers.
* The server and pipeline tests that load a compiled score run once per
  backend. Most engine tests build their database from hand-written Cypher,
  which only Ladybug loads, and stay Ladybug tests: they test the stages,
  which are the same code on either backend.
* The oracle and a server test run on natively loaded scores too
  (`oracle.native.*`, `server.native.*`).
* Timings, measured on 2026-10-25 on `macos-max` in server mode (load is
  `PUT /v1/score`, render is every frame), median of three, the same tables
  on both backends:

  | Score | Backend | Load | Render | Total |
  | --- | --- | --- | --- | --- |
  | echoes (5,120 partials, 8 frames) | Ladybug | 5.94 s | 1.67 s | 7.61 s |
  | echoes | native | 0.047 s | 0.126 s | 0.17 s |
  | convergence (800 partials, 20 frames) | Ladybug | 3.20 s | 1.91 s | 5.10 s |
  | convergence | native | 0.012 s | 0.71 s | 0.72 s |

  Every frame of both backends is byte-identical to the frames the engine
  rendered before the stores were introduced. What remains of convergence's
  time is its kernel, which is synthesis rather than data access.

## More Information

* AGENTS.md ("LadybugDB is the graph store", Tech Stack, the `engine-db`
  component row) and architecture/overview.md §5.2 ("A Ladybug graph
  database") describe `engine-db` as Ladybug. Their wording is for the author
  to change once this is accepted.
* [ADR 0022](0022-batch-engine-db-writes-as-row-lists.md), whose batching
  becomes the Ladybug implementation of the write contract.
* [ADR 0025](0025-compile-scores-to-parquet-tables-keep-cypher-for-debugging.md),
  the Parquet tables native mode reads.
* `rendering-engine/docs/integrity.md`, the checks that stay on Ladybug.
* Revisit validation in native mode if scores start arriving from sources
  other than the compiler.
