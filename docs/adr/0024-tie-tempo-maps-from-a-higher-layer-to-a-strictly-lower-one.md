---
status: accepted
date: 2026-10-17
---

# Tie tempo maps in HzL from a higher layer to a strictly lower one, and keep one map per timeline

## Context and Problem Statement

The engine resolves a graph of tempo maps tied by
`TEMPO_MAP_SYNCHRONIZATION_POINT` edges (ADR 0023). The graph must be acyclic,
and every map must lead to its Score's root map. HzL has to let a
practitioner write several maps, tie them together, and say which map each
sound keeps time by. Two questions follow: what keeps the ties from forming a
cycle in HzL, and where is a map chosen? The language is in
`hzl-compiler/docs/hzl-reference.md` §4.1, §5.1 and §9.

## Decision Drivers

* Practitioners who are not careful about structure should still be able to
  put their material into a reasonable order quickly, and be told at once
  when a tie points the wrong way.
* A cycle of ties is a fault that should be impossible to write, not one
  found by searching for it.
* The compiler does not resolve maps to physical time; that is the engine's
  work, and only the engine's.
* Layers have been carried on every HzL file since the proof of concept, with
  nothing enforced; they should not grow into a general dependency system yet.

## Considered Options

* Tie direction by layer: a child's synchronisation point names a parent in
  a strictly lower layer, layers compared as strings.
* No ordering: any map may tie to any other, and the compiler detects cycles.
* Ties declared on the score, in one list.

## Decision Outcome

Chosen option: **tie direction by layer**, because it makes a cycle
impossible by construction, using a field every file already has, and gives
the practitioner an order to arrange material in that the compiler can check
locally, file by file.

* Synchronisation points are written on the child `tempo_map` file, which
  names its parent. A parent's `layer` sorts strictly before the child's.
  The same layer is refused.
* Layers compare as strings, by code point; nothing in the string is
  interpreted. A three-digit prefix (`"000:Root"`, `"001:Voices"`) is the
  convention, for readability only.
* The score's map is the root and has no ties. Every other `tempo_map` file in
  the source tree is compiled and must lead to the root.
* A sound entity keeps time by one map, named on its placement in the score
  and defaulting to the root. An envelope placement may name another map, and
  then gives an absolute position on that map, since the compiler cannot know
  where the entity's start falls in another map's ticks. There is no map per
  stage or point. Each musical timeline therefore keeps time by one map, as
  the engine requires (ADR 0023).
* Layers order tempo-map ties only. No other reference is checked against
  them yet.

Decided with the author on 2026-10-17.

### Consequences

* Good, because a cycle cannot be written, and the error for a wrong tie
  names the two maps and their layers.
* Good, because sorting maps by layer puts every parent before its children,
  so checking that a map leads to the root is one pass.
* Good, because no schema or engine change was needed: the compiled output is
  exactly what the hand-written polytempo examples already were.
* Bad, because a layer is a string, so `"10:x"` sorts before `"9:x"`. The
  three-digit convention is there to avoid that, and nothing enforces it.
* Bad, because an envelope on another map than its entity's gives an
  absolute position, so moving the entity does not move that envelope.
* Neutral, because every map in the tree is compiled, so one tree holds one
  score's maps. Leaving out what a rendering run does not need is a later
  optimisation.

Impact on components:

* `hzl-source` — `synchronization_points` on `tempo_map`; `tempo_map` on a
  sound entity's placement and, with an absolute `position`, on an envelope's.
* `hzl-compiler` — lowers every map of the tree and its ties; checks the
  layer order, the root rule and each tie's position; leaves rules that need
  physical time to the engine.
* `engine-db` — no change.
* `rendering-engine` — no change; it still rejects a timeline whose points
  lead to different maps.

### Confirmation

`hzl-compiler/tests/test_tempo_maps.py` covers each rule. `make
test-hzl-oracle` compiles the HzL twins of `polytempo-shift-score` and
`polytempo-stretch-score`, renders them and checks them against the
hand-written scores' oracles.

## Pros and Cons of the Options

### Tie direction by layer (chosen)

* Good, because it uses an existing field, and a wrong tie is caught in the
  file that makes it.
* Bad, because string order is less forgiving than a number would be.

### No ordering, with cycle detection

* Good, because any arrangement of files works.
* Bad, because a cycle is only found once the whole graph is read, and the
  practitioner gets no help keeping material in order.

### Ties on the score

* Good, because the whole graph is in one place.
* Bad, because a map's ties are part of what the map means; moving them out of
  its file makes the map harder to reuse and leaves layers unused.

## More Information

* `hzl-compiler/docs/hzl-reference.md` §4.1, §5.1, §9.
* `rendering-engine/docs/polytempo.md` §4, §6.
* ADR 0023, for how the engine resolves the graph.
