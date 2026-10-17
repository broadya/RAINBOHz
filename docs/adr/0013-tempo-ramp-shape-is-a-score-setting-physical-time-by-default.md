---
status: accepted
date: 2026-10-17
---

# The tempo ramp shape is a Score setting, linear in physical time by default, and a tempo map's validity rules

## Context and Problem Statement

A `TempoMap` is a list of `TempoMapPoint`s. Each sets the tempo at the start
of the segment it opens, and `hold` says whether that tempo holds to the next
point or ramps to the next point's tempo. The transform stage
(`rendering-engine/docs/transform.md` §6.1) converts tick positions on the map
to seconds, so "ramps" has to mean one precise thing, and the map has to be
well formed enough that every position has a defined time.

There are two ways to read a linear ramp. The tempo can be a linear function of
**physical time** (BPM against seconds, dT/dt constant), or of **musical
position** (BPM against beats, dT/db constant). They give different elapsed
times for the same segment: 100 → 140 BPM over four beats takes 2.000 s by
physical time and 2.019 s by musical position.

This ADR first chose musical position (2026-10-02), on the grounds that it is
what Logic, Cubase, Pro Tools and Live do. That claim did not hold up (see
the survey below). The author thinks of a ramp as dT/dt constant. So the shape
became a setting, with physical time as the default (author, 2026-10-17).

## Decision Drivers

* Practitioner familiarity. The canonical model is meant to feel like a DAW,
  and the playback plugin will sit inside one, but DAWs differ, so no single
  shape matches all of them.
* The conversion must have a closed form. Precomputation and rendering rely on
  exact, repeatable arithmetic, and a numerical integration per point would be
  both slower and harder to reproduce in an oracle.
* Every tick position must map to one time, including positions after the
  last point, so that a score need not say where it ends.

## Considered Options

* Linear in musical position only.
* Linear in physical time only.
* One switch on the `Score` that chooses the shape for every ramp in it.
* A shape per segment, on `TempoMapPoint`.

## Decision Outcome

Chosen option: **one switch on the `Score`**,
`tempo_ramp_physical_time BOOLEAN`, with physical time as the default:

* NULL or true: every ramp is linear in physical time. This is REAPER's
  behaviour and the author's mental model.
* false: every ramp is linear in musical position.

One switch is enough. A piece written for a DAW follows that DAW's shape
throughout, and both shapes have closed forms. A per-segment shape would cost
a schema column on every point, and nothing yet needs it.

With B = b₂ − b₁ beats between points (b₁, T₁) and (b₂, T₂), and Δt the
seconds from b₁ to beat b:

**Linear in physical time** (dT/dt constant):

```
D     = 120 · B / (T₁ + T₂)                    the segment's duration
T(τ)  = T₁ + (T₂ − T₁) · τ / D                 the tempo τ seconds in
T(b)² = T₁² + (T₂² − T₁²) · (b − b₁) / B        the tempo at beat b
Δt(b) = 120 · (b − b₁) / (T₁ + T(b))            the beats at their mean tempo
b(Δt) = b₁ + Δt · (T₁ + (T₂ − T₁) · Δt / (2D)) / 60
```

The Δt form is exact, has no cancellation as the ramp flattens, and needs no
special case: at T₁ = T₂ it is 60 · (b − b₁) / T₁. dT/db is not constant.

**Linear in musical position** (dT/db constant):

```
T(b)  = T₁ + (T₂ − T₁) · (b − b₁) / B
Δt(b) = 60 · B · ln(T(b) / T₁) / (T₂ − T₁)      when T₁ ≠ T₂
Δt(b) = 60 · (b − b₁) / T₁                     when T₁ = T₂
b(Δt) = b₁ + T₁ · B · (exp((T₂ − T₁) · Δt / (60 · B)) − 1) / (T₂ − T₁)
```

dT/dt = (dT/db) · T / 60 is not constant.

In both shapes a held tempo, or a ramp between equal tempi, is computed as
60 · (b − b₁) / T₁. So a map without ramps gives bit-identical times whatever
the setting.

The validity rules, unchanged since 2026-10-02 (author):

* `position` is in ticks of the owning map, from the map's start, and
  positions strictly increase.
* The first point is at position 0: time zero of the Score is tick 0.
* The last point must have `hold = true`. There is no next tempo to ramp to,
  and the tempo simply continues. A one-point map is therefore valid and
  common: "this tempo forever".
* Tempo is in the schema's [1, 1000] BPM; `ticks_per_beat` is positive.

### The DAW survey

This was gathered on 2026-10-17. The confidence varies, and each line says
how much of it rests on measurement.

| Host | Shape | Confidence |
| --- | --- | --- |
| REAPER | physical time | Measured: 100 → 140 BPM over four beats takes 2.000 s |
| Cubase | probably physical time | Not verified |
| Pro Tools | unknown | Not verified |
| Logic, Live | probably musical position | Inferred, not measured |
| Standard MIDI Files | steps only | Format: tempo events are steps; a ramp is exported as many steps |
| DAWproject | undefined | The format names a "linear" tempo segment without saying in what |

**To confirm a host:** draw a ramp from 100 to 140 BPM over four beats and
read the time at its end. 2.000 s means physical time, 2.019 s means musical
position.

### Consequences

* Good, because the default matches how the author, and REAPER, think of a
  ramp, and a score for a position-based host can still say so.
* Good, because both conversions are closed-form: one square root or one
  logarithm per segment, exact to a few ulp, and both are reproduced
  independently by the oracle's Python mirrors.
* Bad, because one score cannot mix the two shapes. That would need a shape
  per segment.
* Bad, because a reader of a tempo map must know the Score's setting to
  interpret `hold = false`. The REST API therefore returns it with the root
  map.
* Neutral, because the last-point rule is an integrity constraint the schema
  cannot express; `--validate-db` and the transform both check it.

Impact on components:

* `rendering-engine`:
  * The transform reads `Score.tempo_ramp_physical_time` and builds each
    `TempoMap` with a `TempoRamp`. It converts ticks to seconds with
    `secondsThrough()` and refuses any map that breaks a rule.
  * `--validate-db` reports such a map.
  * The root tempo map in the REST API carries `tempo_ramp_physical_time`,
    with NULL resolved to true.
* `engine-db` — `Score.tempo_ramp_physical_time` (nullable). `TempoMap` and
  `TempoMapPoint` carry the ramps as `hold` between points. The last-point rule
  is not expressible in the schema, so it is checked in code.
* `hzl-compiler`:
  * The `score` file has an optional `tempo_ramp: physical_time |
    musical_position`, default `physical_time`, which is always written to the
    Score.
  * Its tempo maps must meet the rules: first point at tick 0, strictly
    increasing positions, tempo in [1, 1000] BPM, and the last point held.
* The DAW plugin (not built yet) — sets the switch to match its host, and maps
  the host's seconds back to beats with the matching closed-form inverse.

### Confirmation

* `rendering-engine/test/transform/test_tempo_map.cpp` checks both formulas
  (including the 100 → 140 case) and every rule.
* `test_timing_query.cpp` and `test_transform.cpp` check that the Score's
  setting reaches the conversion.
* `rendering-engine --validate-db` reports a map that breaks a rule
  (`integrity.md` §3), and the transform refuses to run on one.
* Two oracle tests check a rendered ramp against values computed outside the
  engine: `tempo-map-score` (physical time, the default) and
  `tempo-map-position-score` (musical position).

## Pros and Cons of the Options

### Linear in musical position only

* Good, because it matches Logic and probably Live, and has a closed form.
* Bad, because a tempo's change per second is not constant, which is not how
  the author or REAPER think of a ramp.

### Linear in physical time only

* Good, because a tempo's change per second is constant, and it has a closed
  form.
* Bad, because a score written for a position-based host would play its ramps
  at different times.

### One switch on the Score

* Good, because each score follows the host it was written for, with one
  nullable column and no change to tempo maps.
* Bad, because one score cannot mix the shapes.

### A shape per segment

* Good, because a score could mix shapes.
* Bad, because every point needs another column, and no piece needs it yet.

## More Information

* `rendering-engine/docs/transform.md` §6.1, and `secondsThrough()` in
  `rendering-engine/src/transform/tempo_map.cpp`.
* ADR 0012 for the distinction between musical and physical time this
  conversion crosses.
* `rendering-engine/docs/polytempo.md`, the polytempo design, which works in
  both shapes.
* Revisit with a shape per segment if one piece needs both.
* `engine-db/schema/engine-schema.cypher`, the `Score`, `TempoMap` and
  `TempoMapPoint` definitions.
