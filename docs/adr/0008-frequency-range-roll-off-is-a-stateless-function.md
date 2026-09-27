---
status: accepted
date: 2026-09-24
decision-makers: Alan Broady
consulted: Claude (design discussion)
---

# The frequency-range roll-off is a stateless function of instantaneous frequency

## Context and Problem Statement

The schema header defines a rendering range around the audio range with a
roll-off to silence at each end, so that a partial glided out of range fades
rather than clicking or aliasing. During the rendering design
(`rendering-engine/docs/rendering.md` §8.2) the question arose of how the
roll-off should respond to frequency that changes rapidly across a band
edge. Two mechanisms were considered, one of which carries state per
partial.

## Decision Drivers

* **No feedback, no history.** AGENTS.md: "Partial state is a pure function
  of time, with no history-dependent stepping." This is an architectural
  invariant, not a preference.
* **Random access.** Any frame must be renderable without the frames before
  it.
* **No transients of its own.** The roll-off exists to remove clicks; it must
  not introduce any.
* **Hot-loop cost.**

## Considered Options

1. **A per-partial fade counter.** A dB value, normally 0, decremented by
   1 dB per out-of-range sample and incremented by 1 dB per in-range
   sample, saturating at −144 dB and 0 dB. 144 consecutive out-of-range
   samples silence the partial: about 1.5 ms at 96 kHz.
2. **A stateless ramp.** A function of the signed instantaneous frequency
   alone: 0 dB in the audio range, linear in dB to −144 dB at the outer edge
   of each transition band, −144 dB beyond. Added to the per-sample dB sum.

## Decision Outcome

Chosen option: **2**, because option 1 violates the first two drivers and
fails the third.

The counter is history-dependent stepping: its value at a frame's first
sample depends on samples before the frame. In most cases that is
recoverable by looking back for a run of 144 same-status samples, but an
input that alternates status without ever saturating carries its initial
state indefinitely, and then frame *k* cannot be rendered without frame
*k−1*. It is also a hard-edged fade — 1 dB per sample is a 12 percent
amplitude change per sample, with a corner at each end — which is a
broadband transient of exactly the kind the roll-off is meant to prevent.

The stateless ramp has neither problem. Frequency is continuous in time in
this model, so the amplitude the ramp produces is continuous in time. The
two cases where it changes fast are benign: a steep nominal gradient changes
amplitude at the rate the pitch is changing, which the listener hears
regardless; a frequency modulator oscillating across a band edge produces a
tremolo at the modulator's rate, which the schema keeps subsonic.

### Consequences

* Good, because the roll-off is a pure function of the partial's state at
  the instant, as everything else in the model is.
* Good, because it costs two comparisons and, only inside a band, one
  multiply-add per partial per sample, and composes with the rest of the
  amplitude by addition in dB.
* Good, because the counter's value at a frame start would otherwise have
  needed an analytic reconstruction from band crossings, which is possible
  on piecewise-linear frequency but is machinery the engine does not need.
* Bad, because a frequency that changes very fast produces an amplitude
  that changes very fast. This is accepted: in that case the sound is
  already a transient.
* Neutral, because the ramp is linear in dB rather than in amplitude, as the
  schema header's wording might suggest; linear in dB composes additively
  and reaches the −144 dB floor at the band edge. The header's "linear
  roll-off" is read as a shape, not as a domain.

### Confirmation

`amplitude_maths` has no state; `rollOffAt` takes a frequency and the band
edges and nothing else, and its tests evaluate it at points, not sequences.
The per-partial hot-loop state in `render_kernel` is phase, frequency and
nominal amplitude only (`rendering.md` §7.1); a fourth per-sample state
would be visible in review.

## More Information

* `rendering-engine/docs/rendering.md` §8.2.
* `engine-db/schema/engine-schema.cypher`, "Units and limits", for the
  ranges.
* Negative frequency is treated as below the range and so silent; a
  reversed-sine interpretation is deferred to a much later stage
  (`rendering.md` §14). That would be a separate decision.
