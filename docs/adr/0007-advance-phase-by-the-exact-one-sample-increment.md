---
status: accepted
date: 2026-09-24
---

# Advance the render-time phase accumulator by the exact one-sample increment, re-anchored at every chunk

## Context and Problem Statement

Rendering keeps a wrapped phase accumulator per partial and advances it once
per receiver sample (`rendering-engine/docs/rendering.md` §7). Precomputation
has already integrated each partial's frequency exactly, so every integrable
segment carries the exact unwrapped phase at its start
(`rendering-engine/docs/precomputation.md` §6.5). The question is what the
accumulator adds per sample, and whether it is ever allowed to carry an
accumulated value across a boundary. The two must agree, or the phase the
renderer reaches at a boundary differs from the phase precomputation stored
there, and the re-anchoring at the next segment produces a phase jump.

## Decision Drivers

* **Continuity with precomputation.** The renderer must arrive at each
  boundary with the phase precomputation stored for it, to the last bit,
  or the model's "same maths for precomputation and rendering" rule
  (schema, `PartialSegmentNominalFrequency`) is broken at every boundary.
* **Random access.** Frames rendered separately must join without
  discontinuity, so no error may compound across frames.
* **Cost in the hot loop.** One addition per partial per sample is the
  budget; anything more is paid 10¹⁰ times per second of audio at scale.
* **Generality.** Curves, frequency modulators and Doppler must not change
  the form of the step.

## Considered Options

1. **Add the instantaneous frequency** `f(τ_n)` at the sample. The
   forward-Euler step, and the natural first reading of "increment the phase
   by the frequency".
2. **Add the exact one-sample increment** `Φ(τ_n + 1) − Φ(τ_n)`, the
   difference of the segment's antiderivative, and re-anchor the accumulator
   from the closed form at the start of every chunk.
3. **No accumulator**: evaluate `phaseAt(startPhase, nominal, τ_n)` directly
   at every sample.

## Decision Outcome

Chosen option: **2**, because it is the only one that meets the continuity
driver at the cost of option 1. For a linear segment the increment is
`f(τ_n) + g/2`: the same one addition as option 1, with a per-segment
constant folded in. Option 1 drifts from the exact integral by `g/2` per
sample, which over a one-second glide is a visible phase jump at the next
boundary. Option 3 is exact but costs a multiply-add per sample more than
necessary and, more importantly, offers nothing the accumulator with
anchoring does not.

Re-anchoring at every chunk, not only at every segment start, is part of the
decision: no accumulated value ever survives a boundary, so error is bounded
by the chunk length and never by the position in the score.

### Consequences

* Good, because the renderer's phase at every boundary is bit-identical to
  precomputation's, by construction rather than by tolerance.
* Good, because the step generalises: for any richer `Φ` (curves, modulator
  terms) the increment is still `Φ(τ+1) − Φ(τ)`, and under Doppler it is
  `Φ(τ+dτ) − Φ(τ)` with the partial set's per-sample rate `dτ`. Nothing in
  the loop's shape changes.
* Good, because two frames rendered in separate runs agree at their join to
  the same bound as two adjacent samples in one chunk.
* Bad, because the step is *not* "add the frequency", and the `g/2` term
  looks like a mistake to a reader who expects it to be. The code and the
  design document both say why it is there; this record exists so that a
  later "simplification" removing it is recognised as a regression.
* Neutral, because the accumulator is a genuine loop-carried dependency per
  partial. Lanes across partials are independent, so this does not limit
  SIMD throughput; a layout with lanes across samples would use option 3
  per lane instead, and the kernel boundary permits either.

Impact on components:

* `rendering-engine` — the render kernel steps each partial's accumulator by
  `Φ(τ+1) − Φ(τ)`, using the same `Φ` as precomputation (`segment_maths.hpp`),
  and re-anchors it from the closed form at the start of every chunk.
* `engine-db` — the exact unwrapped start phase that precomputation stores on
  each integrable segment is what the renderer anchors to. It has to stay
  exact, not rounded for storage.
* `playback-server` (not built yet) — frames rendered separately, by one
  instance or by several, join without a phase discontinuity, so it can sum
  and sequence frames from any instance.

### Confirmation

`test_render_kernel` compares the accumulator against `phaseAt` evaluated
directly at every sample of a chirp, and against precomputation's stored
phase at a segment boundary; the oracle (`rendering.md` §11.1) compares whole
frames against closed-form phase with no accumulator at all. Both fail on
option 1.

## Pros and Cons of the Options

### 1. Instantaneous frequency

* Good, because it is what every textbook oscillator does and needs no
  explanation.
* Bad, because it is a first-order integration of a function that
  precomputation integrated exactly; the discrepancy at boundaries is
  systematic, not rounding, and grows with the segment's gradient and
  length.

### 2. Exact one-sample increment, re-anchored per chunk

* Good, because exact for the segment's closed form at the same cost.
* Good, because anchoring makes every chunk independent.
* Bad, because it needs a per-segment constant and a sentence of
  explanation.

### 3. Direct evaluation per sample

* Good, because it has no loop-carried dependency and no drift at all.
* Bad, because it recomputes `f_s τ + ½ g τ²` per sample, and with curve and
  modulator terms that grows to several transcendental evaluations where
  the accumulator needs one addition.

## More Information

* `rendering-engine/docs/rendering.md` §7.2, §7.3 and §10.
* `rendering-engine/docs/precomputation.md` §6.5 and §8, for the
  antiderivative and why it is the shared definition.
* `rendering-engine/include/rainbohz/rendering_engine/segment_maths.hpp`,
  where `Φ` lives.
* Revisit if a sample-lane SIMD layout is adopted (option 3 per lane), or if
  profiling shows the anchoring cost of very many chunks (`rendering.md`
  §14).
