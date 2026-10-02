---
status: accepted
date: 2026-10-02
decision-makers: Alan Broady
consulted: rendering-engine/docs/transform.md
informed: rendering-engine, engine-db, hzl-compiler
---

# Resolve physical-time collisions by semantic order: the later state wins, and only the tail yields

## Context and Problem Statement

A point of a musical timeline is placed by `POINT_AT`: a tick position on the
tempo map plus `offset_seconds`, a fixed physical offset. The offset is what
lets a percussive sound keep its length at any tempo. It is also what lets
points land out of order once converted: a hit that lasts 200 ms, repeated
every 150 ms, puts the start of the second hit before the end of the first.
That is valid input. What physical time does each point get?

## Decision Drivers

* The order of points in the timeline is the composer's intended order of
  states. It carries meaning that the computed times do not.
* Physical timelines must have strictly increasing times
  (`precomputation.md` §7), and never negative ones.
* The rule must be a single forward pass with a result that does not depend on
  how far back a collision reaches, so that it is cheap, deterministic and
  easy to reason about.

## Considered Options

* Semantic order over time: a colliding point takes the tail's time, replaces
  its state, and later points are compressed into the room left.
* Report a collision as an error and leave the composer to fix it.
* Retrigger: a colliding point sits at its own early time and removes every
  placed point later than it.
* Independent clamping: every point takes `max(own time, tail time)` with no
  compression.

## Decision Outcome

Chosen option: semantic order over time (author, 2026-10-02). Walking the
points in order with a tail at time T:

1. A point later than T is placed at its own time.
2. Otherwise the run of points that keep stepping backwards collapses to its
   last member, which replaces the tail's state at the tail's time. By the
   argument of limits, as a gap shrinks to nothing the later point sits
   immediately after the earlier one; and two states at one instant would be
   a change in zero time, which means nothing, so the later wins.
3. The points up to the next point later than T are compressed in proportion
   into `[T, that point]`, so a shortened attack keeps its shape.
4. Only the tail yields. Points placed earlier are never moved or removed,
   however early a later point computes: a later hit is delayed until the
   earlier one ends.

Time zero is a floor handled by the same rule. Squashing is done per partial
and per property, over only the points where that property has a value, and
the frequency envelope then defines the partial's lifeline. The full statement
is `rendering-engine/docs/transform.md` §6.3 and §6.4.

### Consequences

* Good, because the composer's order is guaranteed: every state they wrote is
  heard in that order, or replaced by a later one, never reordered.
* Good, because the result is strictly increasing and non-negative by
  construction, in one pass.
* Bad, because at the collision the sound jumps: the earlier state is cut by
  the later one with no overlap. An overlap or cross-fade may come later; the
  author has deferred it.
* Bad, because a later state can arrive later than written (it waits for the
  tail), which is the price of never touching placed points.
* Neutral, because a compressed stretch can collide again inside itself; the
  same rule applies, and the loop handles it.

### Confirmation

`rendering-engine/test/transform/test_squash.cpp` states each case of the
rule, including the floor, a decreasing run, compression, a nested collision
and the absence of a later point. The `offset-collision-score` example renders
three overlapping hits and the oracle checks it against placements worked out
by hand.

## Pros and Cons of the Options

### Semantic order over time

* Good, because meaning is kept and the rule is local and deterministic.
* Bad, because of the jump at a collision.

### Report an error

* Good, because nothing surprising is ever rendered.
* Bad, because overlapping fixed-length sounds at a faster tempo are a normal
  musical situation, not a mistake.

### Retrigger

* Good, because the later hit starts exactly when written.
* Bad, because earlier states are discarded, so the composer's order is not
  what is heard; and a collision can reach arbitrarily far back.

### Independent clamping

* Good, because it is simpler still.
* Bad, because a compressed attack loses its shape: every point stacks on the
  tail instead of being spread into the room left.

## More Information

* `rendering-engine/docs/transform.md` §6.3, §6.4 and §14; `squash()` in
  `rendering-engine/src/transform/squash.cpp`.
* Revisit when the overlap at a collision is designed, and when the
  `sync_point` order rule (non-decreasing, allowing equal ticks) is confirmed
  or tightened.
