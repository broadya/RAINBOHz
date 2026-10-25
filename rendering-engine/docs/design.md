# rendering-engine — component design

**Status:** mostly a stub. Records what the architecture already fixes and what
is still open. Of the six responsibilities in §1, the first, part of the
second, and first versions of the third, fifth and sixth are implemented; see
§3. The third, the transform, has its own design in
[transform.md](transform.md); the fifth, precomputation, in
[precomputation.md](precomputation.md); the sixth, rendering, in
[rendering.md](rendering.md).

This is the component-level document that
@see architecture/overview.md §2 anticipates:
"Component-level detail belongs in a separate document per component."

---

## 1. Responsibility

`rendering-engine` is the component where everything that needs the graph
database happens. It owns the database, loads it, transforms it, and computes
audio from it.

It is the successor to `render-orchestrator`, which dispatched this work to
remote workers over the network. That split was abandoned in overview.md 0.2:
a partial's specification is not much smaller than the work of rendering it, so
shipping the specification somewhere else costs more than it saves. See
overview.md §11.

Six responsibilities, in the order a request meets them:

1. **Host** `engine-db` and encapsulate all access to it. Nothing else in the
   system opens a graph database.
2. **Load** `canonical-form` into it, and set dirty flags from the dependency
   information in the graph.
3. **Transform** the canonical model into the rendering model, in place. Musical
   time becomes physical time; Hertz becomes radians per sample; the
   spatial-audio timing work happens here.
4. **Determine the calculation delta** — what, if anything, this request
   actually requires.
5. **Precompute** phase integration over integrable segments, and perform phase
   compensation.
6. **Render** frames to multi-channel WAV objects, write them to
   `object-storage`, and record the references in the graph.

## 2. What is settled

| Decision | Source |
| --- | --- |
| Implemented in C++ | overview.md §9, "Settled" |
| Hosts `engine-db` in process | overview.md §5.2, §9 |
| One physical database, not two | overview.md §5.2 |
| Sharded by partial set | overview.md §5.2 |
| Performs the transform, precomputation and rendering itself | overview.md §5.2 |
| Writes WAV objects to `object-storage`, never returns audio payloads | overview.md §8 |
| The render path operates exclusively in physical time | overview.md §8 |

Three constraints carry directly into the design:

- **Idempotency is critical.** Parallel requests targeting the same partial set
  may trigger duplicate precomputation. That must be safe: all operations on a
  given version of the graph produce the same outputs from the same inputs.
  Write contention is a performance concern, not a correctness one. This
  invariant has to survive every design decision made here.
- **No feedback.** Nothing in the engine may introduce ordering dependencies
  between partials that the model does not have. Partials are largely
  independent, but they are not self-contained descriptions of audio: they share
  properties held above them, such as a `SoundSource`'s spatial position
  (ADR 0020).
- **Random access.** Any frame is renderable in isolation from precomputed
  phase, without rendering the frames before it. This is demonstrated
  empirically by `proof-of-concept/maths/phase_integration_render_demo.py`.

## 3. Requests

A request names a **frame**, a **label set**, and a **source version**. Frames
are contiguous sequential integers covering roughly one second of audio each;
frame numbering is predictable and crosses component boundaries, so it is part
of the contract rather than an internal detail.

Three ingresses, one per run mode:

- **`message-bus`** in production (`distributed` mode) — a Kafka topic. This
  is the real path, and it is not implemented.
- **REST calls** in `server` mode: `PUT /v1/score` names a score, `POST
  /v1/frames/{frame}/renderings` names one frame and a label set, and GET calls
  read the score's tempo maps and engine parameters. The engine stays up
  between calls and keeps the loaded score in memory. See
  [server.md](server.md) and the OpenAPI document
  [api/openapi.yaml](../api/openapi.yaml).
- **A YAML file** in `batch` mode, the test harness, so the engine can be
  exercised without standing up anything. It carries the same fields.

Correspondingly, two egresses: `object-storage` in production, and a local file
in `batch` and `server` modes. The server returns each file as a `file://` URI,
so that its callers do not change when the URI later names an object instead.

The batch harness is not a lesser path. Until the orchestration subsystem is
more than a sketch (overview.md §5.4), it and the server are the only paths
that can actually be run, and it is the one the oracle tests use. The two run
the same steps through the same code (`score_pipeline.hpp`), and the server's
end-to-end tests check its frames with the same oracle.

### 3.1 What is implemented

The engine takes a `--mode` of `batch`, `server` or `distributed`. `batch` and
`server` run; `distributed` is recognised and rejected, so that the mode axis
exists in the code rather than arriving later as a retrofit. (`batch` was
called `test` until server mode arrived, and a fourth mode, `standalone`, a
server writing to a local directory, was reserved; `server` is what it became.)

`server` mode is specified in [server.md](server.md). Each `PUT /v1/score` runs
the loading steps below, up to precomputation, into a fresh in-memory
`engine-db` that replaces the one before; each rendering call runs the label
check and renders one frame; the GET calls only read. Calls are served one at a time
(@see ADR 0016).

Within `batch` mode, the first steps of the batch sequence exist, plus the
reporting that says whether they worked:

- a render request is read from the YAML file named by `--request`, and its
  shape checked, before anything is loaded (§3.2);
- an empty `engine-db` is opened (in memory, per §5; `--db-path` puts it on
  disk as a debugging affordance) and the schema is applied to it from the one
  Cypher file named by `--schema`;
- the score is loaded from the one file named by `--state`: hzlc's
  `manifest.json` of Parquet tables, or a Cypher script (below);
- with `--validate-db`, the integrity of the canonical model that loaded is
  checked (list shapes, and the ranges of the columns the transform reads),
  and the run stops before the transform if a check fails; see
  [integrity.md](integrity.md);
- every partial set and SoundSource with a musical timeline is transformed:
  its points are placed in physical time through the tempo map its
  `POINT_AT`s lead to, one of the Score's synchronised maps; each
  partial gets a `FrequencyPhysicalTimeline`, an `AmplitudePhysicalTimeline`
  and a `PhasePhysicalTimeline`, and each source an azimuth, an elevation and
  a distance timeline. This is the first version, covering no modulators;
  see [transform.md](transform.md). A set, partial or source that cannot be
  transformed stops the run here;
- with `--validate-db`, the physical timelines are checked in turn, and the
  run stops before precomputation if a check fails;
- every partial is precomputed: its `PartialSegmentPhysicalTimeline` of integrable
  segments with their start phases, the frames it is in scope for, and a
  `FrameEntry` per frame leading to where that frame's rendering starts.
  This is the first version, covering linear frequency envelopes only; see
  [precomputation.md](precomputation.md);
- a census reports what the schema declared and what the database now holds,
  row by row, including precomputation's output;
- the request's frames are listed, and every label it names is checked against
  the `Label` nodes that loaded;
- every requested frame is rendered to a 32-bit float WAV file in
  `--out-dir`, created if absent, with a per-partial-set file alongside
  under `--per-set-wav`. This is the first version: mono, linear curves,
  every partial in scope regardless of labels; see
  [rendering.md](rendering.md).

Two files, one each, named separately. There is no directory sweep: which files
a run applies, and in what order, is something the command line says outright
rather than something a directory listing decides. `--schema` defaults to the
in-repo `engine-db/schema/engine-schema.cypher`, baked in at configure time so
that a developer build picks up a schema edit without a reconfigure. `--state`
has no default, because a run must not silently load a score nobody asked for.

The row census matters more than it looks. Statement counts say the file parsed
and the database accepted it; they do not say anything landed, because a
`CREATE` whose `MATCH` found nothing is accepted too and stores nothing. Row
counts are what distinguish a loaded `engine-db` from an empty one.

Cypher reaches the database through an in-process loader, statement by
statement, so that a failure carries its source line — see
@see ADR 0005.

**The two forms of a score** (ADR 0025). `--state` is chosen by its
extension. A `.json` file is a manifest of Parquet tables, hzlc's normal
output: the engine builds a `COPY <Table>(<columns>) FROM '<file>'` for
each node table, in the manifest's order, then a `COPY <REL> FROM '<file>'
(from=…, to=…)` for each relationship pair, and Ladybug reads the rows as
data. A `.cypher` file is a script, as above: hzlc's debugging form
(`--debug-cypher`), or a state written by hand like those in
`engine-db/examples/`. Both forms of one score load the same database
(`make test-score-forms`). The tables are much faster: 0.11 s against 9.2 s
for `echoes`' 82,005 rows. That is on Ladybug; the native backend (§5.1)
reads the same tables itself with Apache Arrow, and is the one a manifest
loads into unless `--backend ladybug` says otherwise.

A manifest is checked whole before anything runs, by the same code for both
backends (`score_manifest.hpp`):

- every table and column name is an identifier;
- every file is a plain `.parquet` name that resolves, symlinks followed, to
  a regular file in the manifest's own directory, so a manifest under
  `--input-root` reads nothing outside it;
- on Ladybug, each node table must be empty before its rows are copied in
  (the native loader starts from an empty model and refuses a table listed
  twice), because
  relationships name their ends by id and the ids are row indices only in a
  fresh table; and its row count must match the manifest's afterwards.

A failure of a table's statement is reported against that table's file,
with no line number.

What the harness does **not** yet have, in the order it will need it:

- **Label selection.** A request's labels are checked for existence and
  then ignored: every partial in a frame's scope is rendered
  (rendering.md §2, §13).
- §1's responsibility 4, and the rest of 3, 5 and 6 -- the features the
  first versions leave out (transform.md §2, precomputation.md §2,
  rendering.md §2). Phase compensation is done for physical timelines
  (precomputation.md §6.8); the transform does not yet map the canonical
  model's phase to them.
- **The production egress.** Files in a directory are the test harness's
  egress; `object-storage` is not written (rendering.md §13).

The `--state` file is also not `canonical-form`. It is tables or Cypher
applied directly to `engine-db`, which sidesteps responsibility 2 ("load
`canonical-form` into it, and set dirty flags") rather than implementing it —
and that is the step overview.md §10 leaves open, so it could not be
implemented yet in any case.
A state file may be written in the canonical model (musical time, as
`engine-db/examples/tempo-map-score.cypher` is) or directly in the rendering
model (physical time, as the first two examples are); the transform converts
the former and leaves the latter alone.

### 3.2 The batch-mode render request

`--request` names one YAML file, required in batch mode. A server rendering
call carries one entry of it, the frame in its path and the labels as JSON,
`POST /v1/frames/1/renderings` with `{"labels": ["strings"]}`, checked by the
same rules:

```yaml
frames:
  - frame: 1
    labels: [strings, brass]
  - frame: 2
  - frame: 3
    labels: []
```

- The top level is a mapping whose only key is `frames`, a non-empty list. A
  mapping rather than a bare list leaves room for keys to be added without
  changing the format.
- Each entry has `frame`, required, and `labels`, optional. No other key is
  accepted, and neither may appear twice.
- `frame` is a plain decimal integer, 0 or greater, that fits INT32, the type
  of `Frame.frame_number`. Frame 0 is the first frame of the score. Quoted numbers, signs, hex, octal and exponents are
  refused, so what the file says is exactly the frame rendered.
- Each frame appears at most once. One frame is one WAV file, so two entries
  for the same frame would collide.
- `labels` is a set of non-empty names, matched against `Label.name`. Order
  carries no meaning and a repeat is ignored. Omitted, null and `[]` all mean
  every partial: an empty list meaning "nothing" would be too easy to write by
  accident.
- A label that no `Label` node carries is an error, not a selection of
  nothing. A misspelt label would otherwise render silence, which looks like
  success. This check needs the loaded score, so it runs after `--state`.
- Every problem is reported with its line, all together, as the Cypher loader
  does.

**The source version is not in the file.** §3 says a request names a frame, a
label set and a source version. In batch mode the version is implicit: it is
whatever `--state` loaded, and there is no other to name. A server is in the
same position: the version is whichever score the last `PUT /v1/score` loaded. When requests arrive
over `message-bus` the version has to travel with them, and this format may
gain a key for it then.

**Frames are indexed from 0.** The `Frame` table defines frame *n* as covering
`[n * FRAME_SIZE, (n + 1) * FRAME_SIZE)`, so frame 0 is the first second of the
score, and the request follows it. Counting from 0 suits programmers and not
users, but a frame is never shown to a user: a user asks `playback-server` for
a time range, and frames are how components divide that range between them.
Numbering that users do see, such as MIDI channels, is where counting from 1
belongs; this is not that.

## 4. Delta determination

The first thing the engine does with a request is decide whether there is any
work to do at all.

**It does not inspect `object-storage`.** The graph holds references to the
rendered objects, and the engine trusts them. The delta comes from dirty flags,
which the load step sets from dependency information already present in the
graph.

This is worth stating as the assumption it is: if the graph and the object store
ever disagree, the graph wins, and the engine will happily report that audio
exists which does not. The alternative — checking the store on every request —
was rejected because it puts a network round trip in front of work that is
usually a no-op. Reconciliation, if it is ever needed, belongs somewhere else.

## 5. Why `engine-db` is in process

The database is derived from `canonical-form` and is transient: there is no
persistence requirement, so an in-memory store is the natural fit: Ladybug in
memory, or the native model of §5.1.
Ladybug supports in-memory and on-disk with the same code, which is one of the
reasons it was chosen over TerminusDB (AGENTS.md, migration rationale).

The scaffolding keeps an on-disk path available as a debugging affordance only:
being able to open a captured `engine-db` in the `lbug` shell and inspect it
is worth the option. It is not part of the deployed configuration.

Being in-process is also the whole point of the 0.2 redesign. The transform
reads and writes the same store, so the score is never serialised in order to be
converted.

### 5.1 Two implementations behind store interfaces

`engine-db` has one schema, `engine-db/schema/engine-schema.cypher`, and two
implementations (ADR 0028, proposed): the Ladybug database, and a native
model held in C++ containers. A run uses exactly one of them, so there is
still one database per engine and no transfer between two stores.

The stages never see which. Each reaches `engine-db` through the store
interface made for it, speaking in the records it reads and the units it
writes:

| Interface | Header | Used by |
| --- | --- | --- |
| `transform::TransformStore` | `transform/transform_store.hpp` | `transformAll()` |
| `transform::TimingSource` | `transform/timing_query.hpp` | the server's tempo-map and engine-config calls |
| `PrecomputationStore` | `precomputation_store.hpp` | `precomputeAll()` |
| `FrameSource` | `frame_gather.hpp` | `renderFrames()` |

`EngineStore` (`engine_store.hpp`) gathers the four, with the census and the
label names. Every stage reads everything it needs in one bulk read, computes
over in-memory data, and writes each partial's or source's output as one
all-or-nothing unit, 64 per call. That was already their shape; the
interfaces made it the contract.

- **Ladybug** (`ladybug_store.hpp`, `ladybug_*.cpp`) holds every stage's
  Cypher, unchanged from before the split, and writes through WriteBatch
  (ADR 0022). Loading a score, `--validate-db` and the schema census stay
  operations on `EngineDb`, because only this backend has them.
- **Native** (`native_store.hpp`, `native_*.cpp`) holds the data in the
  types of `native_model.hpp`. They are written by hand from the schema and
  shaped for the stages: a timeline's points are one contiguous range, so
  `NEXT_POINT` is vector order, and a FrameEntry is one row with its six list
  nodes. Every type, field and relationship pair carries an `@enginedb{…}`
  tag naming what it holds, and `make check-engine-db-mapping` fails unless
  the tags and the schema agree. A score is read from hzlc's Parquet tables
  with Apache Arrow (`native_table_loader.hpp`), the only file that includes
  it. Each read gives what the Ladybug query of the same name gives, in the
  same order wherever the query states one, and each unit is checked whole
  before anything of it changes.

The backend is `--backend native|ladybug`. Without it, the score decides: a
table manifest loads natively, and a `.cypher` script, `--validate-db` or
`--db-path`, which only Ladybug serves, choose Ladybug (`backendFor()`).

The two are kept equal by test. `test/transform/test_store_equivalence.cpp`
runs every compiled example through both and requires every unit, report,
census, gathered record and timing answer equal, and every frame
byte-identical; `make test-score-forms` renders every example natively and
on Ladybug, from tables and from Cypher; the oracle and a server test run on
the native backend too. One rule follows from equality: where a stage depends
on the order of rows a query returns, the order is stated, in the Cypher and
in the native read alike, never left to Ladybug's plan.

Two ids differ between the backends, and nothing reads them but their own
lists: WriteBatch inserts phase points with and without a phase, and nominal
frequencies with and without a linear component, in two statements each, so
Ladybug numbers each group in turn, where the native model numbers them in
list order.

## 6. Open questions

Carried from overview.md §10, plus those specific to this component.

- **The engine schema is incomplete.** `engine-db/schema/` holds the
  rendering-model concepts and the canonical-model concepts, sharing `Score`,
  `Partial` and `PartialSet` (among others) as one set of nodes carrying both
  perspectives. It is intended to be a superset of the canonical model, and is
  not one yet: the modulator musical timelines are still to come.
  `canonical-db/` is considerably out of date beside it.
- **Dependencies are not designed.** `engine-db` is meant to hold everything the
  engine needs: the canonical model, dependency information, precomputation
  output and rendering results. Nothing in the schema yet represents
  dependencies, which incremental transformation needs. See
  @see ADR 0006.
- **Dirty flags have no representation** in either schema, and §4 depends on them
  entirely.
- ~~**Frame size.** `FRAME_SIZE` is referenced by the rendering design schema and
  declared nowhere. "Typically equal to the sample rate" is not a definition.~~
  Defined: `EngineConfig.frameSize`, or one second at `sampleRate` when it is
  NULL ([precomputation.md](precomputation.md) §4).
- **Shard assignment.** How partial sets map to instances, whether that mapping
  is stable, what happens when the instance set changes, and what an instance
  does with a request naming partials it does not own, are all undefined.
- **Concurrency.** Ladybug is embedded and in-process; how many engine threads
  talk to it, and how that interacts with per-partial-set partitioning, is
  unresolved. The rendering design ([rendering.md](rendering.md) §6.2, §7.5)
  fixes the partial set as the unit of parallelism and keeps every database
  read in a gather layer that finishes before any rendering starts, so the
  question is confined to that layer. Rendering's own threads are settled
  (ADR 0026): a frame's partial sets render on `--render-threads` threads
  after the gather, and only the calling thread talks to Ladybug.
- ~~**The definition of an *integrable segment*** and how its boundaries are
  chosen are not specified.~~ Specified in
  [precomputation.md](precomputation.md) §6.3–6.4: boundaries are the merged
  change points of every input, and segments run between consecutive ones.
- ~~**Phase compensation placement.** Whether it is expressed as OpenCypher against
  `engine-db` or as C++ over extracted values is not settled.~~ Settled
  2026-10-03: C++, in precomputation
  ([precomputation.md](precomputation.md) §6.8,
  @see ADR 0019).
- **Job lifecycle and backpressure.** At target scale (1M+ partials) an instance
  has a great deal of concurrent work in flight. What limits it, and how partial
  failure is handled, are not designed. Idempotency makes retry safe in
  principle; the mechanism is still needed.

## 7. Boundaries this component must not cross

- **The timebase boundary — internal to this component.** Only the transform
  stage converts between musical time and physical time. Nothing in the render
  path may acquire knowledge of the tempo model.

  This used to be a component boundary, enforced by `db-transformer` being a
  separate program. It is now enforced by convention and review. The transform
  is the namespace `rainbohz::rendering_engine::transform` and the directory
  `transform/` under `include`, `src` and `test`; the musical unit types and
  the tempo map live there and nowhere else ([transform.md](transform.md)
  §10). Treat a tempo-aware type reaching the render path as a defect of the
  same severity as a correctness bug, because the architecture no longer has
  any way to stop it.

- **The network boundary.** Specifications and URLs cross it; audio payloads do
  not. Audio moves through `object-storage`.

- **The shard boundary.** An engine instance computes from the partials it owns.
  If rendering a frame ever requires data from another shard, either the sharding
  is wrong or the partial-independence principle has been violated.
