---
status: accepted
date: 2026-10-02
decision-makers: Alan Broady
consulted: rendering-engine/docs/transform.md, engine-db/schema/engine-schema.cypher
informed: rendering-engine, engine-db, hzl-compiler
---

# Tempo ramps are linear in musical position, and a tempo map's validity rules

## Context and Problem Statement

A `TempoMap` is a list of `TempoMapPoint`s. Each sets the tempo at the start
of the segment it opens, and `hold` says whether that tempo holds to the next
point or ramps to the next point's tempo. The transform stage
(`rendering-engine/docs/transform.md` §6.1) converts tick positions on the map
to seconds, so "ramps" has to mean one precise thing, and the map has to be
well formed enough that every position has a defined time.

Two readings of a linear ramp exist. Tempo can be a linear function of
**musical position** (BPM against beats), or of **physical time** (BPM against
seconds). They give different elapsed times for the same segment.

## Decision Drivers

* The canonical model is meant to feel like a DAW to a composer, and the
  playback plugin will sit inside one.
* The conversion must have a closed form. Precomputation and rendering rely on
  exact, repeatable arithmetic, and a numerical integration per point would be
  both slower and harder to reproduce in an oracle.
* Every tick position must map to one time, including positions after the
  last point, so that a score need not say where it ends.

## Considered Options

* Linear in musical position.
* Linear in physical time.

## Decision Outcome

Chosen option: linear in musical position, because it is what Logic, Cubase,
Pro Tools and Live do with a tempo ramp drawn on a beat-based tempo track, and
because its integral is closed-form. Standard MIDI Files have only step
changes, and DAWs export a ramp as many steps at tick positions, which is the
same reading.

With the tempo between points (b₁, T₁) and (b₂, T₂) being
`T(b) = T₁ + (T₂ − T₁)·(b − b₁)/(b₂ − b₁)`, the seconds from b₁ to b are

```
60 · (b₂ − b₁) · ln(T(b) / T₁) / (T₂ − T₁)      when T₁ ≠ T₂
60 · (b − b₁) / T₁                             when T₁ = T₂
```

The second form is the held tempo and the limit of the first. The inverse,
from seconds to beats, is also closed-form, which the playback side will want.

The validity rules decided with it (author, 2026-10-02):

* `position` is in ticks of the owning map, from the map's start, and
  positions strictly increase.
* The first point is at position 0: time zero of the Score is tick 0.
* The last point must have `hold = true`. There is no next tempo to ramp to,
  and the tempo simply continues. A one-point map is therefore valid and
  common: "this tempo forever".
* Tempo is in the schema's [1, 1000] BPM; `ticks_per_beat` is positive.

### Consequences

* Good, because a composer moving between a DAW and HzL meets one notion of a
  ramp.
* Good, because the conversion is one logarithm per segment, exact to a few
  ulp, and reproduced independently by the oracle's Python mirror.
* Bad, because a ramp that should be linear in seconds (a tempo that falls by
  one BPM per second, say) cannot be written directly. It would need a
  different `hold` kind, which the schema can gain later.
* Neutral, because the last-point rule is an integrity constraint the schema
  cannot express; `--validate-db` and the transform both check it.

### Confirmation

`rendering-engine/test/transform/test_tempo_map.cpp` checks the formulas and
every rule. `rendering-engine --validate-db` reports a map that breaks a rule
(`integrity.md` §3), and the transform refuses to run on one. The
`tempo-map-score` oracle test checks a rendered ramp against values computed
from the formula outside the engine.

## Pros and Cons of the Options

### Linear in musical position

* Good, because it is the DAW convention and has a closed-form integral.
* Bad, because tempo change per second is not constant.

### Linear in physical time

* Good, because tempo change per second is constant, which some think of as
  "linear".
* Bad, because it is not what the tempo tracks composers know do, and the
  musical position of a point then depends on solving for time first.

## More Information

* `rendering-engine/docs/transform.md` §6.1, and `secondsThrough()` in
  `rendering-engine/src/transform/tempo_map.cpp`.
* ADR 0012 for the distinction between musical and physical time this
  conversion crosses.
* Revisit if a second ramp shape is wanted; the natural place is a third value
  of `hold`, which would then be better named.
