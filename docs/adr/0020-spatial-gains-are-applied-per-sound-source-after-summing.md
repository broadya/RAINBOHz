---
status: accepted
date: 2026-10-04
decision-makers: Alan Broady
consulted: rendering-engine/docs/rendering.md, proof-of-concept/spatial-audio
informed: rendering-engine, engine-db
---

# Apply spatial gains once per SoundSource, after summing its partials in mono

## Context and Problem Statement

The engine now renders to as many channels as `EngineConfig.channelMode`
has outputs: one for mono, up to four for first-order Ambisonics. Each
channel's gain comes from a SoundSource's position, which is given by its
`AzimuthPhysicalTimeline`, `ElevationPhysicalTimeline` and
`DistancePhysicalTimeline`. A SoundSource is defined as a group of partials
that share one spatial position (engine-schema.cypher). Where in rendering
should the gains be applied: per partial in the hot loop, per partial set, or
once per source?

## Decision Drivers

* The hot loop's cost per partial per sample is what limits scale
  (rendering.md §8.4). Anything added there is multiplied by the partial
  count, and a million partials are expected.
* All partials of a source share one position, by definition. The gains are
  linear and the same for every partial of the source, so
  Σ g·xᵢ = g·Σ xᵢ, exactly up to rounding.
* Under Doppler the delay depends on the distance, which is per source. So
  the source clock (rendering.md §3.1) belongs to the source too.
* The gains come from the spatial-audio proof of concept, which is the oracle
  for them and computes them from a position alone (direction gain × 1/r).

## Considered Options

* Per partial, in the kernel: each partial's sample multiplied by C gains
  and added into C channels.
* Per partial set: each set's mono buffer spread over the channels by its
  source's gains.
* Per SoundSource, after summing: each set renders to mono as before, the
  sets of a source are summed into one mono buffer, and that buffer is spread
  over the channels once.

## Decision Outcome

Chosen option: "Per SoundSource, after summing", because it is the only one
whose cost does not grow with the number of partials or sets. The mono kernel
is unchanged, and the spatial stage costs a few transcendentals per sample
per source.

### Consequences

* Good, because spatialisation costs O(samples × channels) per source,
  however many partials the source has.
* Good, because the mono partial kernel, its SIMD plan (rendering.md §7.5) and
  its tests are untouched. The spatial kernel `spatialiseChunk()` is a
  separate, small function over a mono span.
* Good, because the place the source clock will need under Doppler already
  exists: `SoundSourcePlan`.
* Bad, because no partial can have its own position. A partial that needs one
  belongs in a SoundSource of its own, which the model already says.
* Neutral, because `--per-set-wav` files stay mono, before the spatial stage,
  by the author's choice. They are what the partials produce, not what is
  heard.
* Neutral, because the mono source buffer is `SampleValue` (`float`), so the
  sum is narrowed once before the gain is applied in double. The extra
  rounding is of the order of one float ulp, as for the sum of sets
  (rendering.md §10).

### Confirmation

`test_spatialise` checks the spatial kernel and driver against
`positionGains()` evaluated directly at every sample. `test_rendering` checks
that two sets of one source are spatialised as their sum, and that two
sources at different positions each get their own gains. The gains
themselves are checked against the proof of concept's golden CSV
(`test_spatial_gains_oracle`).

## Pros and Cons of the Options

### Per partial, in the kernel

* Good, because it would allow a position per partial.
* Bad, because it multiplies the hot loop's output work by the channel count
  and adds C multiply-adds per partial per sample. It also needs the gains in
  the block state per lane, or recomputed per lane, for a quantity that is the
  same in every lane of a source.

### Per partial set

* Good, because it fits the existing per-set loop.
* Bad, because a source with many sets would compute and apply the same gains
  once per set. The sets of a source share the position, so this repeats work
  for nothing.

## More Information

* rendering-engine/docs/rendering.md §8.5 (the spatial stage), §6.2 (who owns
  the source clock).
* rendering-engine/docs/precomputation.md §6.7: precomputation records each
  partial's entry into its source's spatial timelines on its `FrameEntry`.
  The same points are recorded once per partial, and rendering walks them
  once per source.
* proof-of-concept/spatial-audio: the virtual microphones and the eight
  channel modes.
