---
status: accepted
date: 2026-10-01
---

# Distinguish musical time from physical time, in the schema and in the code

## Context and Problem Statement

The system has two kinds of time:

* **Musical time** is measured in beats and measures. In `engine-db` it is an
  integer count of ticks on a `TempoMap`. Where a musical-time point falls in
  seconds depends on the tempo, so it moves whenever the tempo changes.
* **Physical time** is linear time, measured in seconds or, inside the engine,
  in samples. It does not depend on tempo. It is what is usually called
  wall-clock time.

`architecture/overview.md` already said that two time bases exist and must never
be mixed up, calling them *tempo time* and *wall-clock time*. The data model did
not show this. Timelines of both kinds were called `…Timeline` and hung off one
`HAS_TIMELINE` relationship. Whether a timeline was musical or physical could
only be worked out from where it sat in the graph. That is not enough when both
kinds are about to live in one database (ADR 0006).

How should the model and the code show which kind of time a timeline is in?

## Decision Drivers

* `engine-db` is the whole database (ADR 0006). The canonical model's musical
  timelines and the rendering model's physical timelines will both live in it,
  sometimes for the same concept, such as spatial position.
* Converting between the two is allowed only in the transform stage of
  `rendering-engine` (overview.md §8). If that rule is to be kept by
  convention and review, a reader has to see the time base in the name.
* LadybugDB has no inheritance (AGENTS.md, "No Class Hierarchies"). A table's
  name and its relationship tables are the only places a distinction like this
  can live.

## Considered Options

* Put the time base in the names: `…MusicalTimeline` and `…PhysicalTimeline`
  tables, with a separate relationship table for each.
* Keep one set of names and add a discriminator property, such as
  `time_base`, to each timeline.
* Leave the time base implied by which model a table belongs to.

## Decision Outcome

Chosen option: put the time base in the names. A discriminator does nothing
here, because no table holds both kinds. Each timeline table is musical or
physical by definition. A name is visible in every query, every diagram and
every identifier, and the database checks it when an edge is created.

### The vocabulary

* *Musical time* and *physical time* replace *tempo time* and *wall-clock time*
  throughout the documentation.
* Source time and receiver time, the two sides of the Doppler mapping
  (`rendering-engine/docs/rendering.md` §3.1), are both physical time. That
  distinction is a different one and sits inside physical time.
* "Timeline" on its own still means the general idea: a linked list of points
  (AGENTS.md, "Linked Lists"). Code that works on either kind, such as the
  list-integrity checks, keeps the general name.

### The schema

The schema file is the authority. In outline:

| | Musical time (`@canonical`) | Physical time |
| --- | --- | --- |
| Measured in | ticks on a `TempoMap` (`POINT_AT.sync_point`), plus `offset_seconds` | seconds or samples, as a `time` property on each point |
| Owner relationship | `HAS_MUSICAL_TIMELINE` | `HAS_PHYSICAL_TIMELINE` |
| Nominal partial parameters | `PartialSet → PartialSetMusicalTimeline`, one for the whole set | `Partial → Frequency/Amplitude/PhasePhysicalTimeline`, one per partial |
| Spatial position | `SoundSource → SpatialPositionMusicalTimeline` | `SoundSource → Azimuth/Elevation/DistancePhysicalTimeline`, one each per source |
| Modulators | `HAS_MODULATOR_MUSICAL_TIMELINE`, not yet defined | `HAS_MODULATOR_PHYSICAL_TIMELINE` |
| Engine-derived | none | `PartialSegmentPhysicalTimeline`; `FRAME_ENTRY_PHYSICAL_TIMELINE` |

The two kinds differ in more than units. Musical timelines belong to a
`PartialSet`, and keep the relationships between its partials explicit.
Physical timelines belong to each `Partial`, because once the curves are
converted, the partials of a set no longer share the same parameters.

`TempoMap` is not a timeline of either kind. It is what defines musical time.
It holds `ticks_per_beat` itself, and its points are `TempoMapPoint`, with no
separate `TempoMapTimeline` node. At 60 BPM the ticks are subdivisions of a
second, so a practitioner can write milliseconds or samples in musical time
without leaving the musical model.

### What is implemented

The render path reads and writes physical timelines only. The transform
stage (`rendering-engine/docs/transform.md`) reads the musical tables —
`TempoMap`, `PartialSetMusicalTimeline`, `SpatialPositionMusicalTimeline`,
`POINT_AT`, `PartialState` and `SpatialPositionState` — and writes the
per-partial frequency, amplitude and phase physical timelines, and the
per-source azimuth, elevation and distance physical timelines. It is the only
code that reads musical time, and it lives in the namespace
`rainbohz::rendering_engine::transform`, whose unit types (`MusicalTime`,
`MusicalDuration`, `TicksPerBeat`, `Tempo`) are the musical-time counterparts
this ADR anticipated. Only the modulators are still physical-only: their
musical timelines are not yet in the schema.

### Consequences

* Good, because the time base of every timeline is visible in the schema,
  every query, the diagrams and every C++ identifier, so mixing them up shows
  in review.
* Good, because a relationship table that admits only one kind cannot hold an
  edge to the other kind, so the database rejects such an edge.
* Bad, because names are longer, and every concept that exists in both models
  has two tables and two relationship tables.
* Neutral, because `canonical-db/` still uses the old names. It is a design
  artefact, and it will be brought into line separately.

Impact on components:

* `engine-db` — every timeline table and its owner relationship carries its
  time base in its name. A concept held in both models has two of each.
* `rendering-engine` — only the transform reads musical time. Its unit types
  live in `rainbohz::rendering_engine::transform` and do not convert to the
  physical ones. Everything after the transform reads physical timelines only.
* `hzl-compiler` — emits musical timelines only, in ticks on a `TempoMap`.
  It never writes a physical timeline.

### Confirmation

`make validate-schemas` loads the schema. C++ identifiers follow the schema
names (AGENTS.md, "C++ Conventions"), and review checks this. In C++ the
musical unit types do not convert to or add with the physical ones, so a
musical value used as a physical one fails to compile; whether a musical type
has leaked out of the transform namespace is a matter for review.

## Pros and Cons of the Options

### Time base in the names

* Good, because the distinction holds without reading any property values, and
  the database checks it.
* Bad, because there are more tables, and every query that names a timeline is
  longer.

### Discriminator property

* Good, because there are fewer tables.
* Bad, because the two kinds have different shapes (ticks through `POINT_AT`
  against a `time` property) and different owners (`PartialSet` against
  `Partial`). One table would have to be the union of both, with columns that
  are always empty for one kind.
* Bad, because the database does not check property values.

### Implied by the model

* Good, because nothing has to change.
* Bad, because it fails exactly when both models share one database, which is
  the situation ADR 0006 set up.

## More Information

* `engine-db/schema/engine-schema.cypher`: header section "Key paths through
  the data model", and the `TempoMap`, `HAS_MUSICAL_TIMELINE`,
  `HAS_PHYSICAL_TIMELINE` and `POINT_AT` definitions.
* `architecture/overview.md`: "Time bases", and §8 on the timebase boundary.
* `rendering-engine/docs/transform.md` for the conversion itself, ADR 0013
  for the tempo ramp, and ADR 0014 for what happens when converted points
  collide.
