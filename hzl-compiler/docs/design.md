# hzl-compiler — design

**Status:** implemented, first version, 2026-10-02; several tempo maps
2026-10-17. The code is under
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
| `score`, `sound_entity`, `sound_source`, `tempo_map`, `envelope`, `trajectory`, `partial_state_set`, `scale`, `harmonic_series`, `virtual_partial_state_set`, `partial_state_set_transformation` | Anything else |
| Frequency (Hz), amplitude (dB) and phase (degrees, `free`) per partial; spatial position (degrees, metres) per sound source (§5.6) | Spatial position per partial, modulators: refused where written |
| Every tempo map of the tree, tied to the root by synchronisation points (§5.7); a map per sound entity, overridable per envelope | A map per stage or point |
| `Update` in envelope stages; the flattened cartesian product in virtual sets | A product inside a stage (partials born mid-timeline) |
| Each envelope placed once per score | Instancing, which needs an instance-qualified partial name |
| Curves carried per stage, per property | Curves interpreted (the engine renders linear only) |
| Labels emitted for partials and the score | Labels as a selection; labels on entities, sources and sets (`HAS_LABEL` is not declared for them) |
| One Python CLI running every stage in process | The Ninja build; incremental compilation; provenance columns (not in the schema) |
| Parquet tables and a manifest for `COPY FROM`; Cypher, batched by table, for debugging (§7) | Several node tables loaded at once (§7) |

Layers and namespaces are parsed and carried as the proof of concept had
them. One rule uses layers: a tempo map's synchronisation point ties it to a
map in a strictly lower layer (ADR 0024). Nothing else is enforced about
either, deliberately.

## 3. Levels

HzL is lowered level by level, each a pure function from one typed model to
the next:

```
L3  generators     scale, harmonic_series             -> partial_state_set
L2  combinators    virtual_partial_state_set          -> partial_state_set
L1  timelines      envelope                           -> a partial set's musical timeline
                   trajectory                         -> a source's spatial position timeline
L0  tempo maps     every tempo_map of the tree        -> the maps and their ties
    score          score + sound levels               -> canonical model, as tables
    emit           canonical model                    -> .cypher
```

L3 and L2 live in `lower/state_sets.py`, behind a `StateSetResolver` that
dispatches on a referenced file's type, caches results, and refuses a cycle
of references. L1 is `lower/timeline.py` and `lower/trajectory.py`, L0
`lower/tempo_maps.py` and `lower/score.py`, the model
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

Files are parsed with libyaml (`yaml.CSafeLoader`), which is about eight times
faster than PyYAML's pure-Python parser, so hzlc needs a PyYAML built with it
and refuses to start without one. libyaml's syntax-error messages drop the
quoted source line and its `^` marker, so a file it refuses is parsed again
with the pure-Python parser, only to report its error.

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
| `score` | `Score`, with `tempo_ramp_physical_time` from `tempo_ramp` (true unless `musical_position`, always written), `NEXT_POINT → TempoMap` to the root map only |
| `tempo_map` and its points | `TempoMap`, chain of `TempoMapPoint`; one per file in the tree |
| `synchronization_point` | `TEMPO_MAP_SYNCHRONIZATION_POINT` child → parent, `sync_point_child` and `sync_point_parent` in each map's ticks (§5.7) |
| `sound_entity`, `sound_source` | `SoundEntity`, `SoundSource`, with `HAS_SOUNDENTITY`, `HAS_SOUNDSOURCE` |
| `envelope` | `PartialSet` + `PartialSetMusicalTimeline`, `HAS_PARTIALSET`, `HAS_MUSICAL_TIMELINE` |
| initial state set | the partials (`Partial`, `HAS_PARTIAL`) and the first `PartialSetMusicalTimelinePoint`, with a state for every partial |
| each stage | the next point; cumulative duration → `POINT_AT.sync_point`; `offset_seconds` → `POINT_AT.offset_seconds` |
| `Update` in a stage | a `PartialStateSet` with a `PartialState` (`MEMBER_OF_SET_MANY_ONE`, `STATE_OF`) for each partial the stage touched |
| `phase` on a state | `PartialState.phase` and `phase_free_run` (§5.5) |
| stage curve | `*_curve_type`, `*_curve_amount` on the point the stage starts from (§5.3) |
| labels | `Label`, `HAS_LABEL` from `Partial` and `Score` |
| `trajectory` on a source, or none | `SpatialPositionMusicalTimeline` (`HAS_MUSICAL_TIMELINE` from `SoundSource`), its points with `POINT_AT` and one `SpatialPositionState` each (§5.6) |

### 5.1 Time

Every musical position is `beats:ticks` and lowers to
`beats * ticks_per_beat + ticks`, an integer, with the `ticks_per_beat` of the
map it is on. An envelope's start is
`entity.position + source.offset + envelope.offset` on the entity's map, or
the absolute `position` an envelope on its own map gives (hzl-reference.md
§4.1); each point's `sync_point` is that plus the cumulative stage durations,
and its `POINT_AT` leads to that map. Nothing on this
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

### 5.6 Spatial position

Added 2026-10-05, after the engine's spatial rendering and transform
(`rendering-engine/docs/transform.md` §6.8, ADR 0020). The proof of concept
enveloped azimuth, elevation and distance per partial; the engine has one
position per `SoundSource`, applied to the sum of its partial sets. So the
position is not a property of a state set but a timeline of the source's
own, which HzL writes as a `trajectory` file (`hzl-reference.md` §8). The
author chose a file over an inline form: HzL builds larger things from
small reusable ones, and a path is worth reusing.

It lowers as an envelope does, without state sets: the initial position
is the first point, each stage the next, with the stage's curves on the
point it starts from (§5.3) and only the properties the stage states. The
first point starts at the source's start (`entity.position +
source.offset`), so the transform's check that the position is defined
before the source's earliest partial holds by construction.

**Every source gets a timeline.** One that names no trajectory gets one
point at its start, azimuth 0°, elevation 0°, distance 1 m (author,
2026-10-05): the transform requires a position, and supplying a default is
the compiler's job, not the engine's. This is also what the hand-written
`tempo-map-score.cypher` has, so the oracle twin is unchanged. A trajectory
used by several sources is emitted once per source; it makes no partials,
so no naming question arises.

Values are checked in HzL terms (`hzl/common.py`): azimuth [-180, 180]°,
elevation [-90, 90]°, distance [1, 16384] m, refused out of range rather
than wrapped, as the transform does. They are emitted in those units;
the transform converts to radians.

`examples/spatial-score` is the HzL twin of
`engine-db/examples/spatial-musical-score.cypher`, with the partials
written in musical time too. Compiled with `--channel-mode 1` it rendered
to the hand-written score's frames within 2e-12 (2026-10-05);
`tests/expected/spatial-score.cypher` fixes its output.

### 5.7 Tempo maps and their ties

`lower/tempo_maps.py` runs before the score is lowered. It finds every
`tempo_map` file in the tree (`SourceTree.refs_of_type`: every YAML file in a
namespace directory, parsed once and cached for the later loads; a file
whose single key is another type, or not HzL at all, is left alone; invalid
YAML is an error, since it could be a map) and checks the ties against each
other: the root has none, every other map has one, each parent is in a
strictly lower layer, and ties to one parent keep their order. Since parents
are in lower layers, sorting by layer puts every parent before its children,
so a map with a valid tie leads to the root and no cycle is possible.

Rules that need the maps in physical time stay with the transform
(`rendering-engine/docs/polytempo.md` §6); the compiler does not resolve
maps. That is also why an envelope on another map than its entity's gives an
absolute position there rather than an offset.

Every map is emitted, used or not. Selecting what a rendering run needs is
a later optimisation, along with leaving out other unused objects.

## 6. The lowest level: tables

`canonical/model.py` holds the canonical model as `engine-db` is shaped: one
row list per node table, one edge list, compiler-assigned local keys. This is
the lowest level of HzL (`architecture/overview.md` §6, "Lowest abstraction
level"), and it is deliberately not Cypher. Cypher is one serialisation of
these rows, and Parquet files for Ladybug's `COPY FROM` are another, over
the same rows (§7). `hzlc compile --dump-canonical` writes the tables as JSON
for inspection.

## 7. Emission

The model is written in one of two forms, which load the same `engine-db`
and render byte-identical frames (`make test-score-forms`, ADR 0025):

- **Tables** (the default): Parquet files and a manifest, which the engine
  loads with `COPY FROM`. Nothing in them is parsed as Cypher.
- **Cypher** (`--debug-cypher`): one script of literal rows, readable and
  loadable by hand, and about 80 times slower to load.

Both depend on the same rule for ids, set out under the Cypher form below:
a relationship names its ends by the model's row indices, which are the ids
a fresh database gives rows in the order they are created.

### 7.1 Tables

`canonical/tables.py` writes the directory `-o` names:

- `manifest.json`: `format` `"rainbohz-score-tables"`, `version` 1,
  `comments`, `engine_config` (the `EngineConfig` properties), `nodes` (each
  node table in `TABLE_ORDER` with its `rows`, and its `file` and `columns`
  if it has columns) and `relationships` (each relationship (FROM, TO) pair,
  in order of first appearance, with its `rows` and `file`);
- `<Table>.parquet` for each node table with columns, one column per row
  field, typed from the field's annotation (STRING, INT64, DOUBLE, BOOL),
  with None as NULL;
- `<REL>.<From>.<To>.parquet` for each relationship pair: `from`, `to` (row
  indices) and the relationship's properties.

A table with no columns (`PartialStateSet`, say) has no file; the engine
creates `rows` nodes. The manifest is written last, so a directory with one
is complete. An earlier output in the directory is replaced, file by file as
its manifest lists them; a directory holding anything else is refused, and
so is `-o x.cypher` without `--debug-cypher`.

**One row group per node file.** Ladybug reads a Parquet file one row group
per task, in parallel, and numbers rows as the tasks finish, so a node file
of several row groups would get ids out of file order and connect the wrong
nodes. It did on `echoes` (2026-10-21): frames differed by more than their
own peak. `parallel=false` is refused for Parquet. Every node file is
therefore one row group, whatever its size; pyarrow's default would split a
table at 1,048,576 rows (`test_a_node_file_is_one_row_group_past_pyarrows_default`).
Relationship files are left to pyarrow, as edge order does not reach the
audio.

The engine checks the manifest before running anything, refuses a node table
that already holds rows, and checks each table's count after the copy
(`rendering-engine/docs/design.md` §3.1).

**Cost.** On `echoes` (5,120 partials, 82,005 rows; macos-max, 2026-10-21)
the tables load in about 0.11 s, against 9.2 s for the Cypher form, of which
49% was Ladybug parsing the literals and 36% binding them. Each node table is
read by one task. Loading tables in parallel would need ids that do not
depend on read order.

### 7.2 Cypher, for debugging

`canonical/cypher.py` writes the `EngineConfig` in a statement of its own,
from the compiler's options. The score follows as **one statement per table,
in chunks of 1,000 rows**, every node table before any relationship:

```cypher
UNWIND [
    {name: "state_sets#A"},
    {name: "state_sets#B"}
] AS r CREATE (n:Partial {name: r.name});

COPY HAS_PARTIAL FROM (UNWIND [
    {from: 0, to: 0},
    {from: 0, to: 1}
] AS r RETURN r.from, r.to) (from='PartialSet', to='Partial');
```

A table with no columns is created by count
(`UNWIND range(1, n) AS r CREATE (n:PartialStateSet)`). A relationship is
split by its (FROM, TO) pair of tables, since a `COPY` names one pair.
Relationship properties (`POINT_AT`'s) are further columns of the `COPY`.
There is a comment with the count before each table and each pair.

**Ids.** A relationship names its ends by id, and the ids are the model's
row indices. That holds because the engine loads every score into a fresh
database, where a `SERIAL` id counts from 0 in the order rows are created,
and the emitter creates them in model order. It is checked to 20,000 rows in
one statement. A script loaded into a database that already holds rows would
connect the wrong nodes. `COPY` finds both ends by primary-key index, so
loading does no scan.

**Two Ladybug details the shape depends on.** A list of struct literals is
typed from its first row, so a bare `null` there makes the column a STRING
and the statement fails. Every NULL is written with its column's type
(`CAST(NULL AS DOUBLE)`), taken from the row dataclass's annotation. List
literals are indexed from 1, so rows are structs read by name (`r.from`), as
`rendering-engine`'s `WriteBatch` does.

**Why batches.** Until 2026-10-13 the score was one statement holding one
`CREATE` clause of comma-separated patterns. Ladybug's binder merges every
pattern of a clause into one query graph
(`QueryGraphCollection::addAndMergeQueryGraphIfConnected`), at a cost that
grows faster than the square of the pattern count. Profiled on
`playground/pieces/convergence` (macos-max, `sample` at 1 ms):

| Partials | One clause | Batched |
|---|---|---|
| 200 | 0.6 s | about 0.1–0.3 s |
| 400 | 1.8 s | 0.4–0.5 s |
| 800 | 7.0 s (5.7 s binding, 0.07 s parsing) | 0.8–0.9 s |
| 1,600 | 71.7 s | 1.6–1.7 s |

Batched, the load costs about 55 µs per row, whatever the chunk size (100 to
20,000 measured). About half of that is parsing the literals and half is
binding them. One `CREATE` clause per pattern is no way out either: Ladybug
(our fork of 0.19.1) crashed with SIGBUS on a statement of 493 `CREATE`
clauses (2026-10-02, `examples/demo`).

**Order and the audio.** The single clause happened to assign ids in
reverse, so the last partial written got id 0. Batched, ids follow the model.
The engine meets partials in id order, so a score with many partials sums
them in a different order than before. Convergence's frames moved by at most
2.4e-7, one float32 rounding step (author's decision, 2026-10-13, to keep
model order). The oracle twin, the spatial twin and `examples/demo` rendered
byte-identical frames. Relationship order does not reach the audio.

**Beyond this.** At target scale (1M+ partials) about 55 to 110 µs × rows
is many minutes of parsing literals, which is why the tables (§7.1) are the
normal output and this form is for debugging.

## 8. Tests

- `tests/`: pytest over the front end, each level and the emitter, using a
  fixture that writes a small source tree. `tests/expected/tempo-map-score.cypher`
  is the golden output for `examples/tempo-map-score`, compared with comment
  lines stripped.
- `make test-hzl-oracle`: compiles `examples/tempo-map-score`, the HzL twin
  of `engine-db/examples/tempo-map-score.cypher`, and the polytempo twins
  `examples/polytempo-shift-score` and `examples/polytempo-stretch-score`,
  renders each with the engine and checks every frame against the oracle's
  JSON mirror of the hand-written score (`rendering-engine/docs/rendering.md` §11.1). Agreement proves the
  compiled route and the test route give the same audio. The oracle runner
  takes `--state` and `--score` separately, so nothing there changed.
- `examples/demo` compiles, loads with `--validate-db` (60 integrity checks)
  and renders.

## 9. Not yet, and what each needs

- **Spatial position per partial.** Spreading one set's partials in space
  (harmonics fanned out, say) conflicts with one position per source. A
  per-partial offset from the source's position is the likely route
  (author, 2026-10-05); it needs the engine to stop applying gains once per
  source (ADR 0020), and is a later step.
- **Spatial modulators.** In the schema; nothing reads them yet.
- **A tempo map per stage or point.** Left out on purpose (author,
  2026-10-17): a map per sound entity, overridable per envelope, is enough.
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
- **Layers as an ordering for other references**, namespaces as a
  dependency rule, the Ninja build and incremental compilation. Deliberately
  untouched; layers order tempo-map ties only.
