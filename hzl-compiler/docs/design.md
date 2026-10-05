# hzl-compiler — design

**Status:** implemented, first version, 2026-10-02. The code is under
[src/hzl_compiler/](../src/hzl_compiler/); the language it reads is in
[hzl-reference.md](hzl-reference.md).

---

## 1. Purpose

`hzl-compiler` is the compilation half of the pipeline in
`architecture/overview.md` §6: it reads `hzl-source` and lowers it to
`canonical-form`, which `rendering-engine` loads into `engine-db`. Until now
the engine's only input was hand-written Cypher in `engine-db/examples/`.
That remains the engine's test route; this is the actual one.

The first version compiles the proof of concept's HzL, moved from seconds to
musical time, with a score, a tempo map and the two levels between a score
and an envelope added. It needs no change to the engine or the schema: what it
emits is exactly what the hand-written scores already were.

## 2. Scope of this version

| In this version | Not yet |
| --- | --- |
| `score`, `sound_entity`, `sound_source`, `tempo_map`, `envelope`, `partial_state_set`, `scale`, `harmonic_series`, `virtual_partial_state_set`, `partial_state_set_transformation` | Anything else |
| Frequency (Hz), amplitude (dB) and phase (degrees, `free`) | Spatial position, modulators: refused where written |
| One tempo map per score | Several maps and synchronisation points |
| `Update` in envelope stages; the flattened cartesian product in virtual sets | A product inside a stage (partials born mid-timeline) |
| Each envelope placed once per score | Instancing, which needs an instance-qualified partial name |
| Curves carried per stage, per property | Curves interpreted (the engine renders linear only) |
| Labels emitted for partials and the score | Labels as a selection; labels on entities, sources and sets (`HAS_LABEL` is not declared for them) |
| One Python CLI running every stage in process | The Ninja build; incremental compilation; provenance columns (not in the schema) |
| Cypher output | Columnar output for `COPY FROM` (§7) |

Layers and namespaces are parsed and carried exactly as the proof of concept
had them, and nothing is enforced about them. That area is deliberately left
as it is.

## 3. Levels

HzL is lowered level by level, each a pure function from one typed model to
the next:

```
L3  generators     scale, harmonic_series             -> partial_state_set
L2  combinators    virtual_partial_state_set          -> partial_state_set
L1  timelines      envelope                           -> a partial set's musical timeline
L0  score          score + tempo_map + sound levels   -> canonical model, as tables
    emit           canonical model                    -> .cypher
```

L3 and L2 live in `lower/state_sets.py`, behind a `StateSetResolver` that
dispatches on a referenced file's type, caches results, and refuses a cycle
of references. L1 is `lower/timeline.py`, L0 `lower/score.py`, the model
`canonical/model.py` and the emitter `canonical/cypher.py`.

Stages are separate functions, not separate processes, so that the Ninja
generator planned in AGENTS.md can wrap each as a target later without
restructuring. The Ninja build is not in this version.

## 4. Front end

`source.SourceTree` finds a file by `(namespace, name)` under the root, by the
proof of concept's `<root>/<namespace>/<name>.yaml` convention, and checks
the header against the location it was found at. `nodes.Node` wraps each
YAML mapping with its location, so every missing key, wrong type, bad value
or unknown key is reported at the exact place in the file (`file: path:
message`). Unknown keys are refused: YAML accepts a misspelt key silently and
the compiler would otherwise ignore it.

Each HzL type is one module under `hzl/`: a frozen dataclass, a parser from a
`Node`, and a loader from a tree and a reference.

The schema's "compiler-enforced" ranges are enforced here (`hzl/common.py`),
in HzL terms, rather than left for the engine to refuse: amplitude in
[-144, 0] dB, tempo in [1, 1000] BPM, the tempo map's shape, `ticks` below
`ticks_per_beat`. The transform's own validation
(`rendering-engine/docs/transform.md` §8) stays as the second line.

## 5. How HzL maps onto the canonical model

| HzL | `engine-db` |
| --- | --- |
| `score` | `Score`, `NEXT_POINT → TempoMap` |
| `tempo_map` and its points | `TempoMap`, chain of `TempoMapPoint` |
| `sound_entity`, `sound_source` | `SoundEntity`, `SoundSource`, with `HAS_SOUNDENTITY`, `HAS_SOUNDSOURCE` |
| `envelope` | `PartialSet` + `PartialSetMusicalTimeline`, `HAS_PARTIALSET`, `HAS_MUSICAL_TIMELINE` |
| initial state set | the partials (`Partial`, `HAS_PARTIAL`) and the first `PartialSetMusicalTimelinePoint`, with a state for every partial |
| each stage | the next point; cumulative duration → `POINT_AT.sync_point`; `offset_seconds` → `POINT_AT.offset_seconds` |
| `Update` in a stage | a `PartialStateSet` with a `PartialState` (`MEMBER_OF_SET_MANY_ONE`, `STATE_OF`) for each partial the stage touched |
| `phase` on a state | `PartialState.phase` and `phase_free_run` (§5.5) |
| stage curve | `*_curve_type`, `*_curve_amount` on the point the stage starts from (§5.3) |
| labels | `Label`, `HAS_LABEL` from `Partial` and `Score` |

### 5.1 Time

Every musical position is `beats:ticks` and lowers to
`beats * ticks_per_beat + ticks`, an integer. An envelope's start is
`entity.position + source.offset + envelope.offset`; each point's
`sync_point` is that plus the cumulative stage durations. Nothing on this
path is a float. `offset_seconds` is passed through as written, the one
physical time in HzL; the transform's squash rule (ADR 0014) handles any
collision it causes, and the compiler does not check for one.

### 5.2 Sparse states

A stage states only the partials its transforms touched, and a source state
only the properties it has. Everything else is absent at that point, which the
transform reads as "no envelope node here for this partial and property"
(`transform.md` §6.2): the envelope continues to the next envelope node. This
is the proof of concept's `Update` made exact, and it is why a stage can
restate a few partials without restating them all. The two bounds the
transform requires, both properties at the first and the last point, are
checked in L1 and reported against the envelope.

### 5.3 Curves

`precomputation.md` §4: the curve on point *n* describes the path from *n*
to *n+1*. A stage's curve describes the path through the stage, so stage
*k*'s curve is written to point *k*, where the stage starts, and the last
point's curve fields are linear and meaningless. Getting this wrong would be
invisible today, since the engine renders linear only, and wrong later.

### 5.4 Names

Objects are named for a reader of the database, not looked up by name:
`Score`, `TempoMap`, `SoundEntity`, `SoundSource` and `PartialSet` carry
their file's `namespace/name`; a `Partial` carries the proof of concept's
`<initial set's namespace>#<identifier>`. The compiler never emits a
`MATCH`, so no name needs an index.

### 5.5 Phase

Added 2026-10-03, after the transform's phase support. HzL has three phase
kinds and the canonical model spells each with two columns
(`hzl-reference.md` §7.7):

| HzL | `phase` | `phase_free_run` |
| --- | --- | --- |
| `phase: <degrees>`, a target | the degrees | false |
| `phase: free` | NULL | true |
| absent, the default | NULL | NULL |

The compiler carries the kind as `Phase` (`hzl/common.py`: a float,
`FREE_RUN` or None) through every level and spells it out only in L0
(`_phase_columns` in `lower/score.py`). The schema's other combinations are
invalid, and the compiler cannot produce them. Ranges are checked where
written: degrees in [-180, 180], including each value a `phase_expression`
gives; an out-of-range value is an error, not wrapped, as for amplitude.

Under `Update` a phase is a property like the others: absent means "not
given", so a later transform's absent phase keeps an earlier one's. The
product rule for two phases is the author's (`combine_phase` in
`lower/state_sets.py`): the mean of two targets, otherwise the stronger
kind, target over `free` over the default.

Nothing is filled in. A partial whose first state is the default gets its
NULL point at the lifeline's start from the transform (`rendering-engine/docs/transform.md` §6.6), not from
the compiler, so a score that says nothing about phase compiles to default
states only and sounds as it did before phase existed.

## 6. The lowest level: tables

`canonical/model.py` holds the canonical model as `engine-db` is shaped: one
row list per node table, one edge list, compiler-assigned local keys. This is
the lowest level of HzL (`architecture/overview.md` §6, "Lowest abstraction
level"), and it is deliberately not Cypher. Cypher is one serialisation of
these rows; a columnar file for Ladybug's `COPY FROM` would be another, over
the same rows, when loading speed matters (§7). `hzlc compile
--dump-canonical` writes the tables as JSON for inspection.

## 7. Emission

The emitter writes the `EngineConfig` in a statement of its own, from the
compiler's options, then the score as **one statement that is one `CREATE`
clause** of comma-separated patterns: every node, then every relationship,
referring to each other by variable. The style follows the hand-written
examples, with a comment per table and per relationship type.

Why one clause rather than one `CREATE` clause per pattern, which is how the
hand-written examples are written: Ladybug (our fork of 0.19.1) crashed with
SIGBUS on a statement of 493 `CREATE` clauses, while the same 493 patterns in
one clause loaded, transformed and rendered. Established by experiment on
2026-10-02 with `examples/demo`; the per-clause depth of the binder is the
likely cause, and the crash was not investigated further. The largest clause
tried is `examples/demo` with 100 harmonics: 802 partials, 2,400 states,
7,236 relationships, about 10,500 patterns and 650 KB of Cypher, which
loaded, transformed and rendered (debug build, 1 min 43 s for the whole run).

This is the known limit of the current output. A score at the target scale
(1M+ partials) is not one Cypher statement. The tables of §6 are the answer:
Ladybug's `COPY FROM` reads CSV and Parquet directly into a node or
relationship table, and the row lists are already in that shape. That work is
not started; its trigger is a score the single clause cannot carry.

## 8. Tests

- `tests/`: pytest over the front end, each level and the emitter, using a
  fixture that writes a small source tree. `tests/expected/tempo-map-score.cypher`
  is the golden output for `examples/tempo-map-score`, compared with comment
  lines stripped.
- `make test-hzl-oracle`: compiles `examples/tempo-map-score`, the HzL twin
  of `engine-db/examples/tempo-map-score.cypher`, renders it with the engine
  and checks every frame against the oracle's JSON mirror of the hand-written
  score (`rendering-engine/docs/rendering.md` §11.1). Agreement proves the
  compiled route and the test route give the same audio. The oracle runner
  takes `--state` and `--score` separately, so nothing there changed.
- `examples/demo` compiles, loads with `--validate-db` (60 integrity checks)
  and renders.

## 9. Not yet, and what each needs

- **Spatial position.** Belongs on `sound_source`, where
  `SpatialPositionMusicalTimeline` hangs in the schema. Refused now.
- **Several tempo maps.** The transform converts through one map per score;
  `TEMPO_MAP_SYNCHRONIZATION_POINT` is refused there too.
- **A product inside a stage.** Partials born mid-timeline. The proof of
  concept sketched it; the transform's lifeline rule already allows a partial
  whose first envelope node is not at the set's first point.
- **Instancing.** Placing one envelope twice needs a naming decision for
  partials; refused now.
- **Labels on entities, sources and sets.** Need `HAS_LABEL` pairs the schema
  does not declare, which is the author's edit.
- **Provenance** (`_source_file`, `_source_hash`, `_build_timestamp`,
  AGENTS.md). Not in the schema; the compiler has every input to hand when
  the columns exist.
- **Layers as an ordering**, namespaces as a dependency rule, the Ninja
  build and incremental compilation. Deliberately untouched.
- **Columnar output** (§7).
