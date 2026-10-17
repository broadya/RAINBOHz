# rendering-engine — integrity checks

**Status:** implemented. The code is
[integrity.hpp](../include/rainbohz/rendering_engine/integrity.hpp) and
[integrity.cpp](../src/integrity.cpp).

The integrity checks cover invariants that the data model in `engine-db`
relies on but the database does not enforce, or cannot. They answer one
question before anything computes from the data: is what loaded actually well
formed?

---

## 1. Purpose

The components upstream are expected to build a sound `engine-db`: the
compiler and the loader create each list fresh, keep values in range, and so
on. Nothing in the database itself backs that up. Ladybug has no `CHECK`
constraints, and its multiplicity constraints are weaker than they look
(AGENTS.md, Linked Lists). The checks are:

- **a development aid.** Hand-written test scores and compiler output under
  development are exactly where these mistakes happen;
- **a backstop.** A defect is reported as a defect, with the node it concerns,
  rather than surfacing later as a wrong phase or a hang.

The checks are separate from precomputation's own validation
([precomputation.md](precomputation.md) §7), and they answer a different
question:

| | Integrity checks | Precomputation validation |
| --- | --- | --- |
| Question | Is the data valid under the schema's model? | Can this version of the engine compute it? |
| Example | `frequency_curve_type` is between 0 and 4 | `frequency_curve_type` is 0, the only curve supported |
| When | On request (`--validate-db`) | Always, for every partial |
| Scope | Whole tables | One partial's inputs |
| On failure | The run stops before precomputation | That partial fails and the run moves on |

## 2. Running the checks

```
rendering-engine --mode batch --state score.cypher --request request.yaml --validate-db
make run-rendering-engine RE_ARGS=--validate-db
```

The flag is named after what it validates. The command line and the render
request are always validated, so a bare `--validate` would not say which.

The checks run once the state has loaded whole, in **two passes** around the
transform (author, 2026-10-05):

1. **Canonical**, before the transform (`checkCanonicalIntegrity`): what the
   transform reads. The `EngineConfig`, the canonical model's columns, set
   membership, the tempo maps, every musical timeline's `POINT_AT` edges and
   order, the `PartialState`s, the first `SpatialPositionState` of every
   spatial timeline, and the canonical lists.
2. **Physical**, after the transform and before precomputation
   (`checkPhysicalIntegrity`): what the transform writes and precomputation
   reads. The physical timelines' columns, lifeline equality, the coverage of
   every SoundSource's partials by its position, and the physical lists.

The split is by table: a check of a `*Physical*` table, or of precomputation's
segments, belongs to the second pass, and everything else to the first.
Before the transform, a score written in the canonical model has no physical
timelines to check; and some rules, such as a source's position being
defined from the start of its first partial, can only be checked once
musical time has become samples, which is the transform's job. A score
written in physical time, with nothing to transform, simply passes the
second pass on what it was given. `checkIntegrity` runs both passes at once,
for a database already in physical form.

Against a partly loaded score the violations would say more about the load
than about the data, so the checks are skipped there, as the label check is.
If any check in a pass fails, the run reports the violations, exits 1, and
goes no further: no transform after the first pass, no precomputation after
the second. With `--verbose` the checks that passed are listed too.

They are off by default because they read the whole of every table they
cover. That is cheap for a test score and not something to pay on every
request.

The checks only read `engine-db`. They never modify it.

## 3. Range checks

Each rule states the values the schema documents as valid for one column. Every
non-NULL value must also be finite. Integer columns are compared as doubles,
which is exact for every integer type involved.

**Only columns the engine currently reads are checked** (decided 2026-09-23).
The engine is built one feature at a time, and a feature brings its columns'
rules with it. A table the engine does not read yet is not checked, even where
the schema documents a range for it.

| Column | NULL | Valid | Source |
| --- | --- | --- | --- |
| `EngineConfig.frameSize` | allowed (means one second) | > 0 | schema |
| `EngineConfig.sampleRate` | allowed | > 0 | schema |
| `FrequencyPhysicalTimelinePoint.time` | no | ≥ 0 | author, 2026-09-23: a partial cannot start before the score does |
| `FrequencyPhysicalTimelinePoint.frequency` | no | any finite value | schema: unbounded in both directions |
| `FrequencyPhysicalTimelinePoint.frequency_curve_type` | no | 0 to 4 | schema's enumeration |
| `FrequencyPhysicalTimelinePoint.frequency_curve_amount` | no | 0 to 1 | schema: 0 linear, 1 curve |
| `EngineConfig.channelMode` | no | 0 to 7 | schema's enumeration |
| `AmplitudePhysicalTimelinePoint.time` | no | ≥ 0 | as for frequency points |
| `AmplitudePhysicalTimelinePoint.amplitude` | no | ≤ 0 | schema; an amplitude above 0 dB is an **error** (decided 2026-09-23): the schema's "values above 0dB will be clipped" is the renderer's defence against bad data, not permission to store it |
| `AmplitudePhysicalTimelinePoint.amplitude_curve_type` | no | 0 to 4 | schema's enumeration |
| `AmplitudePhysicalTimelinePoint.amplitude_curve_amount` | no | 0 to 1 | schema: 0 linear, 1 curve |
| `PhasePhysicalTimelinePoint.time` | no | ≥ 0 | as for frequency points |
| `PhasePhysicalTimelinePoint.phase` | **allowed**: a free-running point | −π to π radians | [precomputation.md](precomputation.md) §7; the transform clamps ±180° to exactly ±π |
| `AzimuthPhysicalTimelinePoint.time`, `ElevationPhysicalTimelinePoint.time`, `DistancePhysicalTimelinePoint.time` | no | ≥ 0 | as for frequency points |
| `AzimuthPhysicalTimelinePoint.azimuth` | no | −π to π radians | schema |
| `ElevationPhysicalTimelinePoint.elevation` | no | −π/2 to π/2 radians | schema |
| `DistancePhysicalTimelinePoint.distance` | no | 1 to 16384 metres | schema |
| `*_curve_type` of the three spatial point tables | no | 0 to 4 | schema's enumeration |
| `*_curve_amount` of the three spatial point tables | no | 0 to 1 | schema |

| `Score.tempo_ramp_physical_time` | **allowed**: NULL means true, ramps linear in physical time | true or false | [transform.md](transform.md) §6.1 (@see ADR 0013); no check, since every value is valid |
| `TempoMap.ticks_per_beat` | no | > 0 | schema |
| `TempoMapPoint.position` | no | ≥ 0 | ticks from the map's start ([transform.md](transform.md) §6.1) |
| `TempoMapPoint.start_tempo` | no | 1 to 1000 | schema's tempo range |
| `TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_child` | no | the position of one of the child's points | [polytempo.md](polytempo.md) §4; checked by "Tempo map synchronisation" below, not a range rule |
| `TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_parent` | no | ≥ 0 | ticks of the parent; likewise |
| `PartialSetMusicalTimelinePoint.frequency_curve_type`, `amplitude_curve_type` | no | 0 to 4 | schema's enumeration |
| `PartialSetMusicalTimelinePoint.frequency_curve_amount`, `amplitude_curve_amount` | no | 0 to 1 | schema |
| `PartialState.frequency` | **allowed**: no envelope node here | any finite value | transform.md §6.2 (author, 2026-10-02) |
| `PartialState.amplitude` | **allowed**: no envelope node here | −144 to 0 | schema's canonical range |
| `PartialState.phase` | **allowed**: free-running or default, as `phase_free_run` says | −180 to 180 degrees | schema's canonical range ([transform.md](transform.md) §6.6) |
| `SpatialPositionMusicalTimelinePoint.*_curve_type` (azimuth, elevation, distance) | no | 0 to 4 | schema's enumeration |
| `SpatialPositionMusicalTimelinePoint.*_curve_amount` (azimuth, elevation, distance) | no | 0 to 1 | schema |
| `SpatialPositionState.azimuth` | **allowed**: no envelope node here, except on the first point (below) | −180 to 180 degrees | schema's canonical range ([transform.md](transform.md) §6.8) |
| `SpatialPositionState.elevation` | **allowed**, as for azimuth | −90 to 90 degrees | schema's canonical range |
| `SpatialPositionState.distance` | **allowed**, as for azimuth | 1 to 16384 metres | schema's canonical range |

The `EngineConfig.channelMode` and `AmplitudePhysicalTimelinePoint` rows arrived
with rendering ([rendering.md](rendering.md) §9, decided 2026-09-24,
implemented 2026-09-25). The `PhasePhysicalTimelinePoint` rows were added on
2026-10-03, with the HzL compiler's phase support: precomputation has read
those columns since phase control (precomputation.md §6.8), and the rule above says a
feature brings its columns' rules with it. The `TempoMap`, `TempoMapPoint`,
`PartialSetMusicalTimelinePoint` and `PartialState` rows arrived with the
transform ([transform.md](transform.md) §9, 2026-10-02), and
`PartialState.phase` with the transform's phase mapping (2026-10-03). The
spatial point rows, and the `channelMode` range of 0 to 7, arrived with the
spatial stage (rendering.md §8.5, 2026-10-04), and the
`SpatialPositionMusicalTimelinePoint` and `SpatialPositionState` rows with
the transform's spatial position (transform.md §6.8, 2026-10-05).
`PartialState.phase_free_run` is a nullable BOOLEAN with no range; what is
checked is its combination with `phase`, below.

Four further checks cover what no single column can:

- **`EngineConfig` is a singleton**, and at least one of its `frameSize` and
  `sampleRate` is set, so that there is a frame size.
- **Set membership.** Every `Partial` is aggregated by exactly one
  `PartialSet`. Ladybug enforces at most one; zero is the defect, and
  rendering needs the set to group by.
- **Lifeline equality.** For every partial with both timelines, the first
  `AmplitudePhysicalTimelinePoint.time` equals the first `FrequencyPhysicalTimelinePoint.time`
  and the last equals the last, exactly. The two timelines define the
  partial's lifeline together; interior points are independent (author,
  2026-09-24). A partial with only one of the timelines is precomputation's
  failure, not this check's. The lists are walked in C++ from their heads,
  stopping silently at a revisited node, since cycles are the list checks'
  business.
- **SoundSource timelines** (2026-10-04, physical pass). Every `SoundSource`
  with partials has an `AzimuthPhysicalTimeline`, an
  `ElevationPhysicalTimeline` and a `DistancePhysicalTimeline`. Each starts
  at or before the earliest start of its partials' lifelines, since a
  source's position is shared by all its partials and must be defined from
  the first instant any of them sounds (author, 2026-10-04 and 2026-10-05).
  The lifeline is the frequency timeline's span. There is no rule for the
  end: after its last point a timeline's value holds (author, 2026-10-05).
  That the times along a spatial timeline strictly increase is
  precomputation's and rendering's to check, as it is for the other physical
  timelines.

Eight more arrived with the transform, covering the musical-time tables it
reads (transform.md §9; decided by the author on 2026-10-02, extended to
spatial timelines on 2026-10-05 and to synchronised tempo maps on
2026-10-17). All are in the canonical pass:

- **Tempo map shape.** Every `TempoMap` has at least one point; the first is
  at position 0; positions strictly increase; `hold` is set on every point
  and true on the last.
- **POINT_AT.** Every `PartialSetMusicalTimelinePoint` and every
  `SpatialPositionMusicalTimelinePoint` has a `POINT_AT` (the schema enforces
  at most one) with `sync_point` ≥ 0 and `offset_seconds` NULL or finite.
  One check per kind of timeline.
- **Tempo map synchronisation.** Every `TEMPO_MAP_SYNCHRONIZATION_POINT`
  edge has both ticks set, `sync_point_parent` ≥ 0 and `sync_point_child`
  the position of one of the child's `TempoMapPoint`s, and no two edges of
  one child share a `sync_point_child`; and the edges form a DAG that
  reaches every `TempoMap` from exactly one Score's own map: a Score's map
  with a parent, a map reached from two Scores, a map no Score reaches (an
  orphan) and a cycle are each a violation ([polytempo.md](polytempo.md)
  §4, §6; author, 2026-10-17). The rules that need the maps in physical
  time, that anchors strictly increase and that every resolved tempo is in
  [1, 1000], are the transform's.
- **Musical order.** Along each `PartialSetMusicalTimeline` and
  `SpatialPositionMusicalTimeline`, `sync_point` is non-decreasing (equal
  ticks with different offsets are how a fixed-length attack is written),
  every `POINT_AT` leads to a `TempoMap` of the Score the set or source
  hangs from (its own map, or one synchronised to it), and all of a
  timeline's points lead to the same map, since ticks on different maps
  cannot be ordered. One check per kind of timeline.
- **PartialState links.** Every `PartialState` is in exactly one
  `PartialStateSet` and is the state of exactly one `Partial`; the schema
  enforces at most one of each.
- **State membership.** A `PartialStateSet` reached from a timeline point
  holds states only of partials in that point's `PartialSet`, at most one
  per partial.
- **PartialState phase.** Every `PartialState`'s `phase` and
  `phase_free_run` are a target (a phase, false), free-running (NULL, true)
  or the default (NULL, NULL). Any other combination is a violation (author,
  2026-10-03; transform.md §6.6).
- **Spatial first state.** The first point of every
  `SpatialPositionMusicalTimeline` has a `SpatialPositionState` that sets
  azimuth, elevation and distance; an empty timeline fails too. After the
  first point a NULL is "no envelope node here"; on the first there is
  nothing to continue from. Supplying a default position is the compiler's
  job (author, 2026-10-05; transform.md §6.8).

**Decided and not yet a check** (rendering.md §9): **frame entries**. Every
`FrameEntry` has exactly one `FRAME_ENTRY`, one `FRAME_ENTRY_PARTIAL` and one
`FRAME_ENTRY_PHYSICAL_TIMELINE` of each pair (the multiplicities enforce "at most
one"; "at least one" is the check); its frame and partial are joined by
`PARTIAL_IN_FRAME`, and every `PARTIAL_IN_FRAME` edge has exactly one
`FrameEntry`; and each list node it leads to is on the list of that partial,
which the database cannot enforce (precomputation.md §6.7). This is the
engine's own output, which the checks run before, so it belongs with the
question in §7 of checking precomputation's output. Rendering's own
validation covers the entries it reads, per frame (rendering.md §9).

## 4. Linked-list checks

Every list type in `NEXT_POINT` is checked, not just the ones the engine
reads. A cycle or a merge is a structural defect in any list, and the check is
the same for all seventeen types. (`FrequencyModulatorPhysicalTimeline` was
missing from the table until the transform's checks were added on
2026-10-02, although the schema declares it; `TempoMap` and
`PartialSetMusicalTimeline` arrived at the same time. The
`PartialPhaseCompensationSegment` list arrived with phase control on
2026-10-03. It is headed by `PartialSegmentPhysicalTimeline`, which also
heads the integrable segments. A timeline that heads more than one list has
its lists' checks named after their node tables, `PartialIntegrableSegment`
and `PartialPhaseCompensationSegment`, so that the names stay distinct.) These checks are about the graph's shape, not the
meaning of any feature, so they do not come under §3's rule. The canonical
lists (`TempoMap`, `PartialSetMusicalTimeline`,
`SpatialPositionMusicalTimeline`) are checked in the canonical pass and the
others in the physical pass (§2).

`NEXT_POINT` is `ONE_ONE`, but Ladybug counts multiplicity separately for each
FROM/TO pair (AGENTS.md, Linked Lists). The database does refuse branches, and
a node with two predecessors from the same pair. It accepts a node with one
predecessor from each pair: a timeline and a point. That allows:

- **Cycles.** A list whose last node links back to its first, or a loop of
  nodes that no timeline reaches at all. These are reported with the node
  ids, in list order, and the timelines, if any, whose head is on the loop.
  A timeline can only reach a cycle through its head: entering one anywhere
  else would need a node with two predecessors from the same pair.
- **Merges.** A timeline whose first node is also the successor of another
  node, so that two lists share a tail. These are reported with the timeline,
  the shared node, its predecessor, and the timeline whose list the
  predecessor is on, where there is one. A head that is on a cycle is reported
  as the cycle's entry, not as a merge.

Both are errors (confirmed by the author, 2026-09-23). A point-to-timeline
membership edge, which would let the schema enforce ownership, was rejected
as overhead on the most numerous nodes in the database (AGENTS.md, Linked
Lists).

Each list type is read with two queries: timeline to first node, and node to
node. The analysis, `findListDefects`, is then a pure function over ids. It
walks each node at most once and sorts ids so that the report does not depend
on hash order.

Orphans are not reported: a node that no timeline reaches, but that is not on
a cycle, is not a defect under this check.

## 5. Output

```
  integrity: 57 of 60 check(s) passed, 3 violation(s)
```

Every violation gets a `FAILED` line in the layout the loader uses, prefixed
with the name of its check. The name states what a passing check establishes:

```
    FAILED   FrequencyPhysicalTimelinePoint.time is not NULL, finite and >= 0: FrequencyPhysicalTimelinePoint 2: time is -3; it must be >= 0
    FAILED   FrequencyPhysicalTimeline lists do not merge into one another: FrequencyPhysicalTimeline 1 starts at FrequencyPhysicalTimelinePoint 1, which also follows FrequencyPhysicalTimelinePoint 0 in the list of FrequencyPhysicalTimeline 0
```

Ids are `SERIAL` ids, as they are in `engine-db` after this load. Each check
keeps the text of its first 20 violations (`kMaxReportedViolations`) and says
how many more there are. The count is always exact.

## 6. Adding a check

Both families are driven by tables in `integrity.cpp`:

- **A range rule** is a row in `rangeRules()`: table, column, whether NULL is
  allowed, and the bounds, each inclusive or not. The check's name and its
  messages are generated from the row. Add a row when a feature starts reading
  a column, and update the table in §3.
- **A list type** is a row in `kListTypes`. It should mirror `NEXT_POINT` in
  the schema, so a new list type in the schema needs a new row here.

A check that is neither a range nor a list shape, such as the `EngineConfig`
singleton, is a function returning an `IntegrityCheck`, called from
`checkCanonicalIntegrity` or `checkPhysicalIntegrity`, according to whether
it checks what the transform reads or what it writes (§2). Range rules and
list types are assigned to a pass by their table's name.

`kListTypes` and the rule tables are spliced into query text, so they must
only ever hold identifiers from the schema, never input.

## 7. Open questions

- **Time ordering along a list.** Strictly increasing times along a timeline
  are checked by precomputation, for frequency and amplitude
  ([precomputation.md](precomputation.md) §7), and by rendering for the
  part of each amplitude list inside a frame ([rendering.md](rendering.md)
  §9). The same invariant holds for every timeline, and it would sit
  naturally here as a third list check.
- **Checking precomputation's output.** The list checks already cover
  `PartialSegmentPhysicalTimeline`'s two lists, but they run before precomputation, so they see
  only output left from an earlier run (`--db-path`). Running the checks again
  after precomputation would check the engine's own output too.
- **Orphans.** Whether nodes that no timeline reaches should be reported
  (§4).
