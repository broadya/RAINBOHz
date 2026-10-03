---
status: accepted
date: 2026-10-03
decision-makers: Alan Broady
consulted: rendering-engine/docs/precomputation.md, rendering-engine/docs/rendering.md
informed: rendering-engine, engine-db, hzl-compiler
---

# Meet phase targets with linear compensation segments, computed in C++ by precomputation

## Context and Problem Statement

A partial's frequency envelope decides its phase: phase is the integral of
frequency. Composers also want to say what the phase must be at certain
times, such as "180 degrees on beat 3, tick 45". They also want to let it
run free, as a consequence of the frequency alone, which is the default and
the common case. A `PhasePhysicalTimelinePoint` `{time, phase}` carries
either a target or `NULL`. How does the engine make the rendered phase meet
the targets, and what does it store so that any frame can still be rendered
on its own?

## Decision Drivers

* Any frame must be renderable in isolation: the phase at any instant must be
  a closed-form function of stored values (design.md §2).
* Precomputation and rendering must agree to the last bit wherever
  precomputation evaluates the phase (precomputation.md §8).
* Meeting a phase exactly generally needs a change of frequency. The change
  should be as small as possible, and the user cannot judge a curve's effect
  on a quantity they do not know (schema, `PhasePhysicalTimelinePoint`).
* "Free-running" must be expressible at any point, not only at the start.
* The total phase should stay a true count of the cycles that occurred.

## Considered Options

* Linear compensation segments: a list of spans, each ramping an additive
  phase adjustment linearly between running totals, independent of the
  integrable segments, computed in C++ by precomputation.
* Phase compensation regions aligned with integrable segments, each segment
  taking an absolute phase at a region's start: the earlier design.
* Compensation expressed as OpenCypher over graph-traversed values (the
  earlier AGENTS.md statement).
* Folding the compensation into the frequency envelope, as an adjusted
  frequency per segment.

## Decision Outcome

Chosen option: linear compensation segments computed in C++ (author,
2026-10-03). The full statement is precomputation.md §6.8. In outline:

1. The natural phase N(t) is the integral of the frequency envelope from the
   initial phase φ₀. φ₀ is held in the integrable segments: it is the first
   segment's `start_phase`.
2. A phase point with a value θ at t moves a running adjustment D by
   `wrap(θ − (N(t) + D))` into **[−π, π)**: the smallest change, with an exact
   half cycle taken as −π. The span ending at t ramps linearly from the old D
   to the new one.
3. A `NULL` point means "free-running up to here". D is unchanged, so the
   span ending there is flat. A `NULL` point is still a boundary, so a later
   target is met over the shorter span.
4. A `NULL` first point makes φ₀ be worked backwards: running free from the
   start meets the first target exactly, with no ramp.
5. D is never wrapped.
6. There is always at least one `PartialPhaseCompensationSegment`, covering
   the lifeline. Its boundaries are the phase points and have no relation to
   the integrable segments'.
7. Rendering adds `D(t)` to the phase. Compensation segment starts are render
   segment boundaries, and the ramp's slope joins the per-chunk phase step
   constant (rendering.md §7.3). The slope is not part of the nominal
   frequency, so the roll-off does not see it.

Every partial has a `PhasePhysicalTimeline` with a point at the start of its
lifeline. Upstream components are to guarantee this, and precomputation
checks it. Rendered frames reach the compensation list through the same
`FrameEntry` as the other lists.

### Consequences

* Good, because the phase at any instant is still closed-form: N from the
  covering integrable segment plus D from the covering compensation segment.
  So a frame needs nothing rendered before it.
* Good, because N(t) at a target is evaluated with the same `phaseAt` on the
  same stored values that rendering uses. The rendered phase meets the target
  up to rounding of the order of 10⁻¹² rad, which the oracle confirms
  independently.
* Good, because the integrable segments stay purely a function of the
  frequency envelope and φ₀. A change of phase targets does not change them.
* Good, because the kernel's cost is unchanged: the slope is folded into a
  constant the loop already adds.
* Bad, because a compensation segment is a list node like any other: one
  more list per partial, one more edge per frame entry, and one more
  far-node read per frame in the gather (the parked hop cost, AGENTS.md
  performance notes).
* Bad, because a long span between two targets can still need a large
  frequency change if the natural phase is far from the target. Nothing
  bounds it; the smallest adjustment keeps it under half a cycle per span.
* Neutral, because phase transitions are always linear. The canonical
  model had `phase_curve_type`, `phase_curve_amount` and `phase_lock` (a
  lock akin to oscillator sync); the author removed all three on
  2026-10-03. Curves would be easy to add but have little musical value,
  and phase lock may return as a feature of its own.

### Confirmation

`test_precomputation.cpp` tests the worked examples of precomputation.md §6.8,
the back-worked initial phase, the half-cycle tie and unwrapped totals.
`test_frame_plan.cpp` and `test_rendering.cpp` check the rendered phase against
the closed form. The oracle (`oracle.phase-target-score`) recomputes the
compensation from its definition in Python and compares every sample of every
frame. `oracle.phase-musical-score` does the same for targets that arrive
from the canonical model through the transform.

## Pros and Cons of the Options

### Linear compensation segments, computed in C++

* Good, because it meets every driver above.
* Bad, because it adds a list to the model.

### Regions aligned with integrable segments

* Good, because there is one list per partial.
* Bad, because a phase target becomes an integrable-segment boundary, so
  changing a target changes the frequency integration, and absolute phases
  at region starts mix the initial condition with compensation.

### OpenCypher over graph-traversed values

* Good, because it keeps arithmetic in the graph.
* Bad, because D is a sequential running total, and N(t) must come from the
  same C++ `phaseAt` as rendering to agree to the last bit. Neither suits a
  query.

### Folding into the frequency envelope

* Good, because the renderer would need nothing new.
* Bad, because the stored frequency would no longer be the composer's
  envelope. It would also feed the roll-off, and it would have to be
  recomputed whenever a target moved.

## More Information

precomputation.md §6.8 and rendering.md §6.4.1 and §7.3 are the design. The
code is `buildCompensation`, `initialPhaseOf` and `naturalPhaseAt` in
`precomputation.cpp`, `compensationAt` in `segment_maths.hpp`, and
`wrapHalfOpen` in `units.hpp`.

The transform maps `PartialState.phase` (degrees, NULL a free-running point)
to the phase timeline (transform.md §6.6, 2026-10-03). To be revisited when
frequency modulators make N(t) richer: they change how N is computed, not
this rule.
