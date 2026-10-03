# Architecture Decision Records

Non-obvious design choices are recorded here, per AGENTS.md, using the
[MADR](https://adr.github.io/madr/) convention (Markdown Any Decision Records,
4.0.0).

## Conventions

- One file per decision, named `NNNN-title-with-dashes.md`, numbers assigned
  sequentially and never reused.
- Start from [`adr-template.md`](adr-template.md). Sections that add nothing for
  a given decision can be dropped; the ones that carry the weight are *Context
  and Problem Statement*, *Considered Options* and *Decision Outcome*.
- Status is one of `proposed`, `rejected`, `accepted`, `deprecated`, or
  `superseded by ADR-NNNN`.
- ADRs record the current situation, not the history of how it was reached.
  When a decision is refined, or the names it uses change, edit the ADR in
  place; git records what changed, when and by whom. A decision that is
  replaced outright is marked `superseded by ADR-NNNN` and points at its
  replacement.
- Record the decision, not the implementation. Implementation detail belongs in
  the code and in component documentation; the ADR explains *why*, and what the
  alternatives cost.

## Index

| ADR | Title | Status |
| --- | --- | --- |
| [0001](0001-build-ladybug-from-pinned-source.md) | Build Ladybug from pinned source and link it statically | Accepted |
| [0002](0002-apply-ddl-one-statement-per-process.md) | Apply LadybugDB DDL one statement per process, and detect failure from output | Accepted |
| [0003](0003-collapse-the-render-pipeline-into-one-component.md) | Collapse the render pipeline into one component, over one database | Accepted |
| [0004](0004-name-the-one-database-engine-db.md) | Name the one database `engine-db`, and build its schema from the rendering model outwards | Accepted |
| [0005](0005-load-cypher-into-engine-db-in-process.md) | Load Cypher into `engine-db` in process, statement by statement | Accepted |
| [0006](0006-retire-rendering-db-engine-db-is-the-whole-database.md) | Retire `rendering-db`; `engine-db` is the engine's whole database | Accepted |
| [0007](0007-advance-phase-by-the-exact-one-sample-increment.md) | Advance the render-time phase accumulator by the exact one-sample increment, re-anchored at every chunk | Accepted |
| [0008](0008-frequency-range-roll-off-is-a-stateless-function.md) | The frequency-range roll-off is a stateless function of instantaneous frequency | Accepted |
| [0009](0009-write-wav-files-with-dr-wav.md) | Write WAV files with a library built for it, dr_wav, not a hand-written writer | Accepted |
| [0010](0010-build-ladybug-from-our-fork.md) | Build Ladybug from our fork, carrying the SERIAL primary-key index fix | Superseded by [0011](0011-carry-upstreams-serial-fix-until-released.md) |
| [0011](0011-carry-upstreams-serial-fix-until-released.md) | Carry upstream's SERIAL primary-key fix in our fork until a release includes it | Accepted |
| [0012](0012-distinguish-musical-and-physical-time.md) | Distinguish musical time from physical time, in the schema and in the code | Accepted |
| [0013](0013-tempo-ramps-are-linear-in-musical-position.md) | Tempo ramps are linear in musical position, and a tempo map's validity rules | Accepted |
| [0014](0014-resolve-time-collisions-by-semantic-order.md) | Resolve physical-time collisions by semantic order: the later state wins, and only the tail yields | Accepted |
| [0015](0015-the-lowest-hzl-level-is-the-canonical-model-as-tables.md) | The lowest HzL level is the canonical model as tables, emitted as Cypher | Accepted |
| [0016](0016-serve-rest-with-cpp-httplib-one-call-at-a-time.md) | Serve REST with cpp-httplib, one call at a time, holding one score | Accepted |
| [0017](0017-rest-resources-under-v1-described-by-openapi-3-1.md) | REST resources under /v1, described by OpenAPI 3.1, with problem+json errors | Accepted |
| [0018](0018-play-audio-with-miniaudio.md) | Play audio with miniaudio, on the default output device | Accepted |
| [0019](0019-phase-targets-are-met-by-linear-compensation-segments.md) | Meet phase targets with linear compensation segments, computed in C++ by precomputation | Accepted |

## A note on the renames

ADRs 0001 and 0002 were written when the C++ component was called
`render-orchestrator`. ADR 0003 renamed it to `rendering-engine`. The component
identifiers in those two files were updated to match, because
`architecture/overview.md` §2 promises that grepping a component's identifier
finds every reference to it, and a dangling name would break that promise.

ADR 0004 renamed the database from `synthesis-db` to `engine-db`, and the
identifier was propagated into 0001 and 0003 for the same reason.

ADR 0006 retired the `rendering-db/` directory. Its references in ADRs 0002,
0003 and 0004 were **not** edited: `rendering-db` was a directory of a design
artefact, never a component identifier, and what those ADRs say about it was
true when they were written. A reader following one of those references will
find nothing there, which is the correct outcome for a record of a decision that
has since been superseded.

ADR 0012 renamed the timeline tables and relationships of `engine-db`
(`FrequencyTimeline` became `FrequencyPhysicalTimeline`, `HAS_TIMELINE` became
`HAS_PHYSICAL_TIMELINE`, and so on), and the terms *tempo time* and
*wall-clock time* became *musical time* and *physical time*. Any ADR using the
old names is updated to the new ones.
