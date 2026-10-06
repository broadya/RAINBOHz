---
status: accepted
date: 2026-09-18
---

# Name the one database `engine-db`, and build its schema from the rendering model outwards

## Context and Problem Statement

[ADR 0003](0003-collapse-the-render-pipeline-into-one-component.md) collapsed the
render pipeline into one component over one database, and named that database
`synthesis-db`. It also left the schema undesigned: the two existing `.cypher`
files, `canonical-db/schema/` and `rendering-db/schema/`, were reclassified as
design artefacts that could not both be loaded, since seven node tables are
declared in both.

Two things follow from that, and this ADR settles both. The name `synthesis-db`
described the merge rather than the thing — the database's defining property is
that it lives inside `rendering-engine`, not that it is synthesised from two
sources. And the merge needed a starting point: an empty schema waiting for a
complete design is not a thing anyone can build against.

## Decision Drivers

* `architecture/overview.md` §2 allows exactly one canonical identifier per
  component and forbids prose variants, so the name has to be settled once and
  applied everywhere in one pass.
* The schema is the core data model and is hand-edited by the author. A starting
  point that is already coherent is worth more than one assembled mechanically.
* The rendering model is the one the engine executes against. Precomputation and
  frame rendering read it directly; the canonical model is what gets loaded and
  transformed into it.
* Both design artefacts remain useful for understanding the problem and are
  cheap to keep loadable.

## Considered Options

* Name it `engine-db` and seed its schema from the rendering model.
* Keep the name `synthesis-db` and seed its schema from the rendering model.
* Keep the name `synthesis-db` and design the merged schema up front, before any
  of it is loadable.

## Decision Outcome

Chosen option: the first. The database is `engine-db`, and
`engine-db/schema/engine-schema.cypher` is the schema it loads. That file starts
as a copy of the rendering-model schema and absorbs the canonical-model concepts
incrementally.

`engine-db` says where the database is and what owns it, which is its one
structural property: it is hosted in process by `rendering-engine` and sharded by
partial set. `synthesis-db` named a construction step that stops being true once
the merge is done. The rename supersedes the name introduced in ADR 0003; nothing
else in that ADR changes.

`canonical-db/` and `rendering-db/` are retained, unchanged, as design artefacts.

### Consequences

* Good, because there is now a schema that exists and loads, rather than one
  deferred until a full merge design is complete. Work on the engine has
  something real to target.
* Good, because starting from the rendering model starts from the perspective the
  engine actually executes against, so the concepts that arrive first are the
  ones the first engine code needs.
* Bad, because `engine-db/schema/engine-schema.cypher` begins byte-identical to
  `rendering-db/schema/rendering-schema.cypher`. Until it diverges,
  `make validate-schemas` validates the same DDL twice and `make docs` generates
  two identical diagrams. The duplication is real but temporary, and the cost is
  seconds of build time.
* Bad, because the canonical model has two homes — the artefact under
  `canonical-db/`, and `engine-db`. `engine-db` now holds the canonical concepts
  (the modulator musical timelines are still to come), and `canonical-db/` is
  considerably out of date beside it, so a reader has to know that `engine-db`
  is the one to trust.
* Neutral, because this decision fixed where the answer to the shared node
  tables would be written down, not what it would be. It has since been
  answered in `engine-db`: shared nodes such as `Score`, `Partial` and
  `PartialSet` carry both perspectives.

Impact on components:

* `engine-db` — the identifier, the `engine-db/` directory, and the schema
  `engine-db/schema/engine-schema.cypher`, grown from the rendering model
  outwards.
* `rendering-engine` — reaches the database through the `EngineDb` wrapper,
  and its code targets the rendering model first.
* `hzl-compiler` — the canonical concepts it emits have to exist in
  `engine-schema.cypher`, and arrive there as they are needed rather than in
  one up-front merge.
* Build system — `make validate-engine-schema` checks the schema loads, and
  `make docs` draws its diagram.

### Confirmation

`architecture/overview.md` 0.3 §5.2 records the resulting shape, and `AGENTS.md`
carries the same framing for agents.

The name is checkable: `grep -rn "synthesis-db\|SynthesisDb\|synthesis_db" .`
over the working tree should return hits only where the rename is itself being
recorded: this ADR, the note in [docs/adr/README.md](README.md), and the 0.3
changelog entry in `architecture/overview.md`.

The schema is checked by `make validate-engine-schema`, which replays the DDL
into a throwaway LadybugDB. That proves it loads. Nothing checks that it is
semantically correct, or that the merge is progressing.

## Pros and Cons of the Options

### Name it `engine-db`, seed from the rendering model

* Good, because the identifier describes a property of the database that stays
  true after the merge completes.
* Good, because `engine-db/` sits alongside `rendering-engine/` and reads as
  belonging to it.
* Bad, because it invalidates a name used throughout ADR 0003, `overview.md` 0.2,
  the architecture diagrams and the C++ scaffolding, all of which had to be
  renamed in one pass.

### Keep `synthesis-db`, seed from the rendering model

* Good, because no rename, and ADR 0003 stays self-consistent without a
  superseding record.
* Bad, because the name describes the transitional act of merging rather than
  what the database is, and becomes stale precisely when the work succeeds.

### Keep `synthesis-db`, design the merged schema up front

* Good, because the first schema written would be the intended one, with no
  interim state in which the canonical concepts are half-present.
* Bad, because it blocks all engine work behind a design question — how seven
  shared node tables reconcile — that is easier to answer against a schema that
  exists than in the abstract.

## More Information

Supersedes the name `synthesis-db` from
[ADR 0003](0003-collapse-the-render-pipeline-into-one-component.md). The
identifier was propagated into 0003 and
[ADR 0001](0001-build-ladybug-from-pinned-source.md) under the terminology-only
exception to ADR immutability — the same treatment the `render-orchestrator` →
`rendering-engine` rename received. What those ADRs decided is unchanged; see the
note in [docs/adr/README.md](README.md).

The decision to seed from the rendering model rather than the canonical one is
the part worth re-examining. If loading `canonical-form` turns out to drive the
schema harder than rendering does, the merge order matters less than it appears
to here — but the answer to the seven shared node tables would still be the same
answer.

Implementation: `engine-db/`, and the `EngineDb` wrapper in
`rendering-engine/include/rainbohz/rendering_engine/engine_db.hpp`.
