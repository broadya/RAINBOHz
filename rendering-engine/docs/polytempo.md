# rendering-engine — polytempo design

**Status:** designed on 2026-10-17, not implemented. The transform still
refuses any `TEMPO_MAP_SYNCHRONIZATION_POINT` edge
([transform.md](transform.md) §2). Ramp shapes follow the Score setting
`tempo_ramp_physical_time` ([transform.md](transform.md) §6.1, @see ADR
0013).

This document describes how several tempo maps in one Score are
synchronised and converted to physical time. It extends the transform
([transform.md](transform.md)), which is the only part of the engine that
reads musical time (@see ADR 0012). The data model is declared in
[engine-schema.cypher](../../engine-db/schema/engine-schema.cypher).

---

## 1. Purpose and scope

**Polytempo** means several streams of music, each with its own tempo
map, sounding at once. Each stream can speed up and slow down on its own,
and the streams meet again at chosen points.

This is not polyrhythm. A polyrhythm, such as three against two, divides a
shared pulse differently, and one tempo map plus the right tick positions
already expresses it. Polytempo changes the pulse itself, independently in
each stream. Polyrhythm needs nothing from this document. Whether
polyrhythm needs anything more is an open question (§10).

The design answers one question: given a child tempo map that is tied to
one or more parent maps at chosen points, where in physical time does every
tick of the child land?

## 2. Motivation

There is a long tradition of music in simultaneous independent tempi:
- Ives's layered bands;
- Nancarrow's player-piano studies;
- Stockhausen's *Gruppen* for three orchestras;
- more recent work for ensembles with click tracks or virtual conductors.

The hard part has always been the arithmetic. Laying out an accelerando is
easy. Laying out one that arrives at an exact point in time, at the same
moment as a different accelerando in another voice, is not (Kocher 2016).

RAINBOHz is not limited to music, so the practitioner's material may not be
music at all. Its musical time is a coordinate system, and polytempo gives
each layer of material its own coordinate system, each tied to the others
at meaningful points. The practitioner should be able to:

- write each layer's tempo map in its own terms, with holds and ramps in
  BPM, as they would in a DAW;
- say "this point of layer B coincides with that point of layer A";
- rely on that coincidence being exact in physical time;
- leave everything else to the engine: the layer stretches or compresses
  between its synchronisation points, as smoothly as possible and only as
  much as needed;
- be told clearly when what they asked for is impossible.

The practitioner's tempo map stays the primary representation (§9 explains
why). The engine adjusts it as little as it can.

## 3. Vocabulary

| Term | Meaning |
| --- | --- |
| tempo map | A `TempoMap` and its list of `TempoMapPoint`s, in ticks |
| point | A `TempoMapPoint`: a position in ticks, a `start_tempo` in BPM, and `hold` |
| segment | The span from one point to the next |
| hold | A segment whose point has `hold = true`: a fixed tempo, then a step to the next point's tempo |
| ramp | A segment whose point has `hold = false`: a continuous change to the next point's tempo. A ramp between equal tempi is flat, but it is still a ramp and can be bent |
| ramp shape | Linear in physical time (dT/dt constant, the default) or linear in musical position (dT/db constant), set by `Score.tempo_ramp_physical_time` |
| parent, child | The two ends of a `TEMPO_MAP_SYNCHRONIZATION_POINT` edge, which runs from child to parent |
| sync point | A point of the child that an edge ties to a parent, at `sync_point_child` |
| anchor | The physical time of `sync_point_parent` on the parent, once the parent has been resolved |
| region | The span of the child between two consecutive sync points |
| interior point | A point of the child strictly inside a region |
| synced span | From the first sync point to the last |
| stretch factor k | A region's nominal duration divided by its anchored duration. k > 1 means faster |
| shared tempo U | The single tempo both regions use at an interior sync point (§5.6) |
| change point | Where a ramp starts or ends inside a segment, after compensation (§5.10) |

Notation, for one segment or region:
- B: its length in beats (ticks ÷ `ticks_per_beat`);
- D: its anchored duration in seconds;
- T: a tempo in BPM;
- U_a, U_b: the tempi at its earlier and later end.

## 4. The model in engine-db

```text
TempoMap ──TEMPO_MAP_SYNCHRONIZATION_POINT──▶ TempoMap
 (child)    sync_point_child  (child ticks)    (parent)
            sync_point_parent (parent ticks)
```

- The edges form a **directed acyclic graph** of tempo maps. Its single
  root is the Score's tempo map (`Score → NEXT_POINT → TempoMap`), which
  plays exactly as written.
- A child may have **several parents**, and a parent may be any map
  further back in its ancestry, not just the one directly above it.
- The DAG says who depends on whom. It is independent of where the sync
  points fall in time.
- Maps are resolved **parents first** (a topological order from the root),
  so every anchor a child needs is already known.
- An edge's anchor is the parent's physical time at `sync_point_parent`.
  The child's resolved time at `sync_point_child` equals the anchor
  **exactly**. This is the overriding rule: wherever continuity or
  smoothness conflicts with an anchor, the anchor wins.
- A child's sync points are taken in order of `sync_point_child`. The
  parents can differ from one sync point to the next.
- Every `sync_point_child` is the position of one of the child's
  `TempoMapPoint`s, so a sync point is always a point, never a position
  inside a segment.

The ramp shape is one setting for the whole Score, and every formula below
exists in both shapes. Physical time is shown first, because it is the
default and it usually has a closed form.

## 5. The cases, from simple to complex

### 5.1 One map

With no edges, the Score's map converts exactly as today
([transform.md](transform.md) §6.1). Per segment:

| | Linear in physical time (default) | Linear in musical position |
| --- | --- | --- |
| tempo | T(τ) = T₁ + (T₂ − T₁)·τ/D | T(e) = T₁ + (T₂ − T₁)·e/B |
| duration | D = 120·B / (T₁ + T₂) | D = 60·B·ln(T₂/T₁) / (T₂ − T₁) |
| time at e beats | Δt = 120·e / (T₁ + T(e)), with T(e)² = T₁² + (T₂² − T₁²)·e/B | Δt = 60·B·ln(T(e)/T₁) / (T₂ − T₁) |

A hold is T₂ = T₁: the time is 60·e/T₁ in both shapes.

### 5.2 One sync point: a pure shift

A child with exactly one sync point is moved in time, without any change of
tempo. Every tick of the child keeps its nominal spacing, and the whole map
slides so that the sync point lands on its anchor.

*Example.* The child is held at 90 BPM. Its beat 0 is tied to beat 8 of a
parent held at 120 BPM. The anchor is 4 s, so the child's beat 0 plays at
4 s, its beat 3 at 6 s, and so on.

### 5.3 Two sync points: one stretched region

With two sync points, the region between them has a fixed number of beats
(B) and a fixed anchored duration (D). If D differs from the nominal
duration, the region is time-stretched by one factor k, applied to every
tempo in it:

- the beats stay where they are in musical time;
- every interior point keeps its tempo relative to the others, T → k·T;
- the interior points keep their relative positions in time.

A uniform stretch keeps everything in proportion in both ramp shapes. By
position, each per-beat gradient scales by k; by physical time, each
per-second gradient scales by k².

*Example.* The child is held at 120 BPM, with sync points at beats 0 and 8.
Nominally that is 4 s, but the anchors are 3.2 s apart, so k = 1.25 and the
region plays at 150 BPM.

### 5.4 Outside the synced span

Number the m segments before the first sync point 0 … m−1:

- Segments 0 … m−2 play at their **nominal** tempi.
- Segment m−1, which ends at the first sync point, changes only if it is a
  ramp. It then ramps from its own nominal start tempo to the tempo the
  synced span has at the first sync point, so it arrives smoothly.
- If segment m−1 is a hold, it keeps its tempo and the music changes tempo
  abruptly at the sync point, a **hard transition**.

The end of the map is the mirror image: only the segment that starts at the
last sync point changes, and only if it is a ramp.

The outer sync points therefore take their tempo from inside the synced
span, and the material outside adapts to it. Because segment m−1's duration
changes, the material before it moves in physical time. That is expected:
only sync points are anchored.

### 5.5 Several regions, and why steps appear

Three or more sync points make two or more regions, each with its own
stretch factor. A uniform stretch per region gives k₁·T on one side of a
shared sync point and k₂·T on the other: a **tempo step** whenever
k₁ ≠ k₂, even though the original map has none there.

This follows from the rules. The beats of each region are fixed, so are its
anchors, and a uniform stretch leaves only k free, which the anchors
already use up.

*Example.* The child is held at 120 BPM, with sync points at beats 0, 4
and 8 anchored at 0, 2 and 3 s. Region 1 fits 4 beats into 2 s (k₁ = 1,
120 BPM), and region 2 fits 4 beats into 1 s (k₂ = 2, 240 BPM). Every
segment is a hold, so nothing can bend, and the tempo steps from 120 to 240
at beat 4. That is the correct result for holds (§5.7).

### 5.6 Compensation at interior sync points

When the segments touching an interior sync point are ramps, the step can
be removed:

1. Compute each region's **uncompensated** factor, kᵢ = nominal ÷ anchored
   duration.
2. At each interior sync point with ramps on both sides, set one **shared
   tempo**, the geometric mean of the two stretched tempi:
   U = T·√(k₁·k₂). A geometric mean suits tempo, because tempo
   relationships are ratios.
3. The ramps that touch the point now end or start at U. Only their
   gradients change.
4. **Solve each region's k again**, so that the region still fills exactly
   its anchored duration, with its interior points scaled by k and its
   boundary tempi fixed.

Step 1 uses the uncompensated factors, so the regions do not couple: each
region's solve in step 4 depends only on its own boundary tempi.

The solve in step 4 always has exactly one answer. As k rises, every
segment of the region gets shorter: the interior ones as 1/k, and the
boundary ramps because one of their ends speeds up. Their total falls
steadily from ∞ (as k → 0) to 0 (as k → ∞). There is no closed form, since
it is a polynomial in k, so it takes one bracketed Newton solve per region,
to machine precision.

*Example.* The same child as §5.5, but with every segment a ramp
(`hold = false`, flat at 120) and interior points at beats 2 and 6.

| | Uniform stretch | Compensated, physical time | Compensated, musical position |
| --- | --- | --- | --- |
| Region 1 (beats 0–4) | 120 | flat at 106.19 to beat 2, then ramps to 169.71 | flat at 107.29, then ramps to 169.71 |
| Beat 4 | step 120 → 240 | 169.71, continuous | 169.71, continuous |
| Region 2 (beats 4–8) | 240 | ramps from 169.71 to 266.66 at beat 6, then flat | ramps from 169.71 to 270.01 at beat 6, then flat |

The anchors are hit exactly in every column. To arrive faster and still
take 2 s, region 1 has to start slower. Compensation can introduce a dip or
an overshoot like this. It is valid and is not reported (author,
2026-10-17).

### 5.7 Holds at sync points

`hold = true` means the practitioner wants that tempo held, so a hold is
never bent:

- **Holds on both sides** of an interior sync point: a hard transition, as
  in §5.5.
- **A hold on one side:** U is the hold's tempo, as solved in the hold's own
  region, and the ramp on the other side meets it. The dependencies run one
  way along the map and never form a cycle.

### 5.8 A region that is a single hold

A region made of one held segment has no interior point, and its tempo is
forced: 60·B/D, in both shapes.

### 5.9 A region that is a single ramp at an edge

The first or last region may consist of one ramp. Its outer end, at the
first or last sync point, has no neighbouring region, so its tempo is free
and the beat count sets it. By physical time it is closed form:
U_outer = 120·B/D − U_inner. By position it takes a Newton solve of the
duration formula. The segment just outside then adapts to the result
(§5.4).

*Example.* The child is flat at 120 with ramps, and has points only at
beats 0, 4 and 8, all of them sync points, anchored at 0, 2 and 3 s.
- U at beat 4 is 120·√(1·2) = 169.71.
- Region 1 starts at 120·4/2 − 169.71 = 70.29 and ramps up to 169.71.
- Region 2 ramps from 169.71 to 480 − 169.71 = 310.29.

*Rejected.* Anchored at 0, 2 and 10 s instead: k₂ = 0.25, so U = 60, and
region 2 would have to end at 120·4/8 − 60 = 0 BPM. The load is rejected
(§6).

### 5.10 A region that is a single ramp between two interior sync points: moving the change point

This region has three fixed quantities: the start tempo U_a at a fixed
time, the end tempo U_b at a fixed time D seconds later, and B beats to fit
between them. A straight ramp has no parameter left to adjust, so it
covers its own beat count, B_ramp = D·(U_a + U_b)/120 by physical time,
which in general is not B.

The method (author, 2026-10-17) **moves the change point**. At one end of
the segment, the tempo carries on as it was just outside the sync point for
h seconds. The ramp then takes the rest of the segment from a new,
computed point. In effect a virtual `TempoMapPoint` is added inside the
segment. It is not a sync point, and it exists only in the transform's
internal map, never in the database. The sync point now sits partway along
a continuous gradient, and the gradient change has moved to the virtual
point. So:

- the number of gradient changes stays the same;
- no step appears at a place with no musical meaning;
- the change point moves away from where the practitioner put it by an
  amount that grows with how far the region is forced.

It is something like what players do when they notice they are out of time
with each other: hold back or push on a little longer, then make the
change.

Write g_L for the gradient of the segment that ends at the earlier sync
point, and g_R for the segment that starts at the later one, both in BPM/s
(0 for a hold). By physical time the h² terms cancel, so h is closed form:

| Candidate | Extension | h |
| --- | --- | --- |
| earlier end, continuing g_L | tempo U_a + g_L·s | (B_ramp − B)·120 / (U_b − U_a − g_L·D) |
| later end, continuing g_R | tempo U_b − g_R·s, counting s back from the end | (B_ramp − B)·120 / (g_R·D − (U_b − U_a)) |
| a fixed-tempo period at either end | g = 0 in the rows above | as above, with g = 0 |

A candidate is valid if 0 < h < D and every tempo it produces is in
[1, 1000] BPM. At h ≥ D the remaining ramp would vanish into a step, and a
negative h means the extension pushes the wrong way. The fixed-tempo
period at the "wrong" end always comes out negative, so it is never
chosen. In effect it sits at the slow end when the region runs too fast,
and at the fast end when it runs too slow.

**Rule:** take the valid candidate with the **smallest h**, the least
forced. If none is valid, reject (§6).

- A gradient candidate helps only if, at the slow end, the neighbour's
  gradient is flatter than the plain ramp, and at the fast end, steeper
  than it.
- A gradient that points the correct way (falling into the segment when it
  runs too fast, rising when too slow) needs a smaller h than a fixed tempo
  at the same end, and the steeper it is, the smaller h gets. A gradient
  between zero and the plain ramp's helps less than a fixed tempo, so the
  rule passes it over.
- The fixed-tempo period is chosen only when no gradient candidate is
  valid. It adds a new gradient change, which is unavoidable then.

*Example.* U_a = 100, U_b = 140, D = 2 s, and B = 3.5 beats. The plain ramp
covers 4 beats, so the region runs too fast.

| Candidate | h | Virtual point | Valid |
| --- | --- | --- | --- |
| fixed tempo at the slow end (100) | 1.5 s | 100 BPM | yes |
| earlier end, g_L = −10 BPM/s | 1.0 s | 90 BPM | yes |
| later end, g_R = +22 BPM/s | 15 s | (−190 BPM) | no: h ≥ D |
| later end, g_R = +60 BPM/s | 0.75 s | 95 BPM | yes: the smallest h, so chosen |

The fixed tempo can win when the gradient candidates fail on range. For
example, 20 → 60 BPM over 2 s needing 1 beat, with g_L = −1000 BPM/s:
h = 0.0196 s, but the virtual point would be at 0.39 BPM, so that candidate
is invalid. The fixed-tempo period (h = 1.0 s) is chosen.

When the extended period is a hold (g = 0), the musical-position shape is
closed form too. The ramp covers c = (U_b − U_a) / (60·ln(U_b/U_a)) beats
per second whatever its length, so h = (D·c − B) / (c − U_held/60). In the
example, a fixed tempo at the slow end gives h = 1.4704 s. With a non-zero
gradient, the position shape is transcendental and needs a bracketed
Newton solve.

If both ends have the same tempo and both neighbours are holds, nothing can
move. The region fits only if 60·B/D equals that tempo, within a tolerance
still to be chosen (§10). Otherwise it is rejected.

### 5.11 Adjacent single-ramp regions

Two single-ramp regions that share an interior sync point might each want
to continue the other's gradient there, which is circular. A joint solve
with one shared gradient has one unknown too many. The natural extra rule,
moving both change points by the same fraction, gave invalid answers in
every case tried, and the remaining freedom tends either to a step or to a
vanishing ramp.

**Rule (author, 2026-10-17):** solve single-ramp regions in order of their
position in the child map. Each continues the **final** gradient of the
region before it, which is already known. At a point shared with a later
single-ramp region, the earlier region may only use a fixed-tempo period.
The tempo is always continuous; the gradient may change at that sync point.

### 5.12 Several parents and the DAG

Nothing above depends on which parent a sync point belongs to. Once every
parent is resolved, each sync point has an anchor time, and the child is
handled as a single list of sync points with anchors (§7). A child of a
child is resolved after its parent's stretch, so its anchors already
include it.

## 6. Validity and early failure

A failure stops the load before rendering, as a fatal error that names the
map, the segment or sync point, and the value. All of these are the same
class of error as playback going backwards in time:

- an edge that makes the graph cyclic;
- `sync_point_child` not on one of the child's `TempoMapPoint`s;
- two sync points of one child at the same tick, or anchored times that do
  not strictly increase in tick order;
- **any instantaneous tempo outside [1, 1000] BPM** after resolution. This
  includes zero and negative tempi, and covers both ends of every segment
  and every virtual point. On a linear ramp, the extremes are at the ends.
- a single-ramp region with no valid candidate (§5.10).

There is no silent fallback: the engine never turns a ramp into a hold or
misses an anchor to get round an impossible request.

## 7. Order of computation

For each map, parents first:

1. Gather the child's sync points from all its edges, in tick order, with
   their anchors from the resolved parents. Check §6's ordering rules.
2. With one sync point, shift (§5.2). With none (the root), convert as
   written.
3. Compute every region's uncompensated k, and U at every interior sync
   point with ramps on both sides (§5.6).
4. Resolve regions that are a single hold (§5.8), and solve the k of every
   region with an interior point (§5.6), in an order that puts each hold's
   region before the ramp that meets it (§5.7).
5. Resolve single-ramp regions in tick order: the edge ones by §5.9, the
   interior ones by §5.10 and §5.11.
6. Adapt the segments just outside the synced span (§5.4).
7. Check every tempo against [1, 1000] (§6).
8. Record the result as the map's internal segments: start time, start and
   end tempo, shape, and any virtual points. Each sync point's time is set
   to its anchor value directly, not reached by summing durations, so the
   sync points coincide bit for bit.

Every later conversion of a tick to seconds on that map works from these
segments, as `TempoMap::toSeconds` does today.

## 8. Computation, accuracy and performance

- **Exactness:** sync times are assigned, not integrated (§7 step 8). Within
  a segment, times are computed from its start, so errors do not build up
  along the map.
- **Closed forms:** every physical-time case except step 4's region solve.
  By musical position, the edge single ramp and any gradient extension need
  a solve.
- **Root finds:** one 1-D solve per region, always bracketed and monotone,
  to machine precision. None of these is an approximation (the author
  values accuracy).
- **Performance:** linear in the number of points and edges, with a small
  constant per region. It runs once per load, before precomputation, and
  nothing on the render path changes. Converting a tick stays a lookup plus
  a closed-form evaluation.

## 9. Comparison with earlier approaches

| Approach | How synchronisation is stated | At a conflict |
| --- | --- | --- |
| One tempo track (MIDI files, mainstream DAWs) | Not at all: a Standard MIDI File format 1 keeps tempo events in its first track, for the whole file. Polytempo is worked out by hand and baked into note positions | — |
| DAW stretch or warp markers on audio | Markers pin audio positions to the timeline | The playback rate changes at a marker. In REAPER it reportedly jumps unless matched by hand (not verified here) |
| Jaffe (1985), time maps | One function per voice from score position to time. Voices synchronise where their maps agree | Left to whoever builds the maps |
| Kocher, Polytempo Composer (2016) | Control points on the time map: an exact (time, position) pair with incoming and outgoing tempi, joined by cubic Béziers | The curve becomes S-shaped, a fluctuation inside the segment. By his own account it gives "no (or only implicit) control over the tempo curve" |
| Antescofo `@target` (IRCAM) | Live: converge on the position and tempo of an upcoming pivot event | A continuous, quadratic tempo adjustment, at performance time |
| **RAINBOHz (this design)** | Edges between tempo maps that the practitioner writes in BPM | Anchors exact. Uniform stretch, then local compensation (geometric mean, moved change points). Rejected when out of range |

Where RAINBOHz sits:

- **The tempo map stays primary.** Kocher's work showed that synchronising
  on a time map is easy, but control of the tempo curve is lost, and he names
  that as his method's weakness. RAINBOHz keeps the practitioner's tempo
  map, holds and ramps, and DAW compatibility. It pays for that with the
  case analysis in §5.
- **Corrections stay local and readable.** A uniform stretch keeps a
  region's internal proportions. Compensation changes only the segments that
  touch a sync point. Moving a change point keeps the number of gradient
  changes. A practitioner can predict the result, which is harder with an
  S-shaped Bézier.
- **Holds mean something.** A hold is never bent, so a hard transition the
  practitioner wrote stays hard. Neither DAW markers nor Bézier time maps
  have a way to say "do not bend this".
- **Impossible is reported, not hidden.** DAWs step, and Béziers fluctuate
  as far as they need to. RAINBOHz rejects any request that would need a
  tempo outside its valid range.
- **Offline and exact.** Unlike Antescofo, it doesn't react to a
  performer. Everything is known in advance, so exact anchors and
  machine-precision solves are possible.

## 10. Open questions

- **Material before time zero.** A child shifted or stretched early can
  place ticks before 0 s, and so can the edge adaptation in §5.4. The
  squash rule (@see ADR 0014) handles points before zero for states, but
  not for a tempo map itself.
- **Equal-tempo tolerance** (§5.10): how close 60·B/D must be to the
  common tempo when nothing can move.
- **Maps with no path to the root**: reject, or treat as a separate root?
- **Polyrhythm beyond this** (§1): whether anything more is needed.
- **Surface syntax** in HzL, and how the server API exposes the resolved
  maps (today `/tempo-maps` returns the stored points; see
  [server.md](server.md) §3.4).
- **A per-segment ramp shape**, if one piece needs both shapes (@see ADR
  0013).

## 11. References

- D. Jaffe, "Ensemble Timing in Computer Music", *Computer Music Journal*
  9(4), 1985, pp. 38–48.
  [CCRMA](https://ccrma.stanford.edu/papers/ensemble-timing-in-computer-music)
- P. Desain and H. Honing, "Tempo curves considered harmful",
  *Contemporary Music Review* 7(2), 1993, pp. 123–138.
  [MCG](https://mcg.uva.nl/mcg-2023/papers/dh-91-c.html)
- H. Honing, "From Time to Time: The Representation of Timing and Tempo",
  *Computer Music Journal* 25(3), 2001, pp. 50–61.
- J. MacCallum and A. Schmeder, "Timewarp: A Graphical Tool for the Control
  of Polyphonic Smoothly Varying Tempos", *ICMC*, 2010, pp. 373–376.
- A. Berndt, "Musical Tempo Curves", *ICMC*, Huddersfield, 2011,
  pp. 118–121.
- P. Kocher, "Polytempo Composer: A Tool for the Computation of
  Synchronisable Tempo Progressions", *Sound and Music Computing
  Conference*, 2016. [Zenodo](https://zenodo.org/record/851241);
  [source](https://github.com/philippekocher/polytempo)
- IRCAM, Antescofo reference, "Synchronization Strategies".
  [antescofo-doc.ircam.fr](https://antescofo-doc.ircam.fr/Reference/time_synchro/)
