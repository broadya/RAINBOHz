---
status: accepted
date: 2026-10-17
---

# Synchronise tempo maps by exact anchors, uniform stretch and local compensation, keeping the tempo map primary

## Context and Problem Statement

A Score may hold several `TempoMap`s, tied together by
`TEMPO_MAP_SYNCHRONIZATION_POINT` edges: a point of a child map is to sound at
the same instant as a point of a parent map. Given such a graph, where in
physical time does every tick of every map fall? The design is
`rendering-engine/docs/polytempo.md`; this record holds the decisions behind
it and what the alternatives would have cost.

The hard part is a child with two or more synchronisation points. The beats
between them are fixed by the map, the seconds are fixed by the parents, and
the child's own tempi, as written, in general fit neither. Something has to
give, and the choice of what gives is the design.

## Decision Drivers

* The practitioner writes tempo maps as in a DAW: holds and ramps in BPM. That
  representation should survive synchronisation, so that the result is
  predictable from what was written.
* Coincidence must be exact. A sync point that lands a sample off its parent
  is a bug, not an approximation.
* Corrections should be as small and as local as the anchors allow, and a
  tempo the practitioner asked to hold must not be bent.
* An impossible request must be reported, never hidden behind a tempo no one
  asked for.
* Accuracy before speed (author); but the resolution runs once per load and
  must not touch the render path.

## Considered Options

* Resolve on the time map (score position against time), as Kocher's
  Polytempo Composer does: control points with incoming and outgoing tempi,
  joined by cubic Béziers.
* Step the tempo at every synchronisation point, as a DAW's stretch markers
  do: stretch each region uniformly and accept the discontinuities.
* Keep the tempo map primary: stretch each region uniformly, then compensate
  locally where ramps meet, and reject what cannot be fitted within the valid
  tempo range.

## Decision Outcome

Chosen option: **keep the tempo map primary, with exact anchors, uniform
stretch and local compensation**, because it is the only option that keeps
the practitioner's holds and ramps recognisable in the result while meeting
every anchor exactly. In outline (polytempo.md §5):

* One synchronisation point shifts the child. Two or more stretch each region
  between consecutive sync points by one factor, every tempo multiplied by it.
* Where ramps meet at an interior sync point, both take the geometric mean of
  the two stretched tempi, and each region is re-solved to hit its anchor. A
  hold is never bent; a hold on one side sets the tempo, holds on both sides
  step.
* A region that is one ramp between two sync points moves its change point:
  one end's tempo, or its neighbour's gradient, continues for a while and the
  ramp takes the rest, so no gradient change is added. The least forced
  valid candidate wins.
* Outside the synced span only the segment touching a sync point adapts, and
  only if it is a ramp.
* Every resolved tempo must lie in the schema's [1, 1000] BPM, anchors must
  increase, every map must descend from exactly one Score's map, and a region
  that cannot be fitted is refused. The load fails; nothing is approximated.

Decisions taken with the author on 2026-10-17 that close the design's open
questions: material shifted before time zero is allowed, and the squash rule
(ADR 0014) places it; a `TempoMap` no Score reaches is a fatal error, not a
second root; equal end tempi between two holds are rejected without any
floating-point tolerance; and a musical timeline keeps time by one map, since
ticks on different maps cannot be ordered.

### Consequences

* Good, because the result is predictable: a stretched region keeps its
  internal proportions, a compensated sync point changes only the segments
  that touch it, a moved change point adds no gradient change, and a hold the
  practitioner wrote stays hard.
* Good, because sync times are assigned from the parent, not reached by
  summing durations, so parent and child agree bit for bit and nothing drifts
  along a map.
* Good, because every step is closed form or a bracketed bisection to
  adjacent doubles, so the resolution is exact to the arithmetic and linear
  in the number of points and edges.
* Bad, because the case analysis is long (polytempo.md §5.3 to §5.11), and a
  compensated ramp can dip or overshoot in a way the practitioner did not
  write. That is accepted and not reported (author, 2026-10-17).
* Bad, because a region whose both ends are pinned has a finite maximum
  duration by physical time, so some anchors that look harmless are refused.
  The error names the region and what it could reach.
* Neutral, because the resolved maps exist only for the transform run.
  Storing them, so that the server and clients can read a child in physical
  time, is a later schema addition (polytempo.md §10).

Impact on components:

* `rendering-engine` — the transform resolves the graph before converting any
  timeline (`transform/polytempo.{hpp,cpp}`); `--validate-db` checks the
  edges' structure; the server's `frame-position` still converts only a root
  map until the resolved maps are stored or recomputed on request.
* `engine-db` — no schema change. The edge and its two sync points were
  already declared.
* `hzl-compiler` — compiles several maps and their synchronisation points,
  ordered by layer (ADR 0024).
* `playback-client` — nothing; it loops in the root map's beats.

### Confirmation

`rendering-engine/test/transform/test_polytempo.cpp` checks every case of
polytempo.md §5 against that document's worked numbers and every rule of §6.
The oracle tests `oracle.polytempo-shift-score` and
`oracle.polytempo-stretch-score` render two example scores and check them
against JSON mirrors computed by an independent script from the formulas, not
by the engine. `test_integrity.cpp` covers the structural checks.

## Pros and Cons of the Options

### Resolve on the time map (Kocher)

* Good, because any set of (time, position, tempo) constraints is met by a
  Bézier, with no case analysis.
* Bad, because control of the tempo curve is lost: a conflict becomes an
  S-shaped fluctuation inside the segment, which Kocher himself names as the
  method's weakness, and the practitioner's holds and ramps disappear.

### Step at every synchronisation point (DAW stretch markers)

* Good, because it is trivial to compute and to explain.
* Bad, because it puts a tempo step where the practitioner wrote a continuous
  ramp, at a place with no musical meaning, every time two regions stretch
  differently.

### Keep the tempo map primary (chosen)

* Good, because the written map stays recognisable and holds mean something.
* Bad, because of the case analysis, and because some requests are refused
  that the other options would have fitted by distorting the curve.

## More Information

* `rendering-engine/docs/polytempo.md`, the full design, with the worked
  examples and the comparison with Jaffe (1985), Kocher (2016) and Antescofo.
* ADR 0012 for the distinction between musical and physical time; ADR 0013
  for the ramp shapes, both of which the resolution supports; ADR 0014 for the
  squash rule that handles material before time zero.
* Revisit when the resolved maps are stored in `engine-db`, when HzL gains a
  syntax for synchronisation, or if a piece needs a correction the local rules
  cannot give (a joint solve across regions).
