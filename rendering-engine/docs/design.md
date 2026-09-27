# rendering-engine — component design

**Status:** mostly a stub. Records what the architecture already fixes and what
is still open. Of the six responsibilities in §1, the first, part of the
second, and first versions of the fifth and sixth are implemented; see §3.
The fifth, precomputation, has its own design in
[precomputation.md](precomputation.md); the sixth, rendering, in
[rendering.md](rendering.md).

This is the component-level document that
[architecture/overview.md](../../architecture/overview.md) §2 anticipates:
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
3. **Transform** the canonical model into the rendering model, in place. Tempo
   time becomes wall-clock time; Hertz becomes radians per sample; the
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
| The render path operates exclusively in wall-clock time | overview.md §8 |

Three constraints carry directly into the design:

- **Idempotency is critical.** Parallel requests targeting the same partial set
  may trigger duplicate precomputation. That must be safe: all operations on a
  given version of the graph produce the same outputs from the same inputs.
  Write contention is a performance concern, not a correctness one. This
  invariant has to survive every design decision made here.
- **No feedback, and partial independence.** Nothing in the engine may introduce
  ordering dependencies between partials that the model does not have.
- **Random access.** Any frame is renderable in isolation from precomputed
  phase. This is what allows a shard to render any frame without consulting any
  other shard, and it is demonstrated empirically by
  `proof-of-concept/maths/phase_integration_render_demo.py`.

## 3. Requests

A request names a **frame**, a **label set**, and a **source version**. Frames
are contiguous sequential integers covering roughly one second of audio each;
frame numbering is predictable and crosses component boundaries, so it is part
of the contract rather than an internal detail.

Two ingresses, one of which is for testing:

- **`message-bus`** in production — a Kafka topic. This is the real path.
- **A YAML file** in the test harness, so the engine can be exercised without
  standing up a broker. It carries the same fields.

Correspondingly, two egresses: `object-storage` in production, and a local file
in the test harness.

The test harness is not a lesser path. Until the orchestration subsystem is more
than a sketch (overview.md §5.4), it is the only path that can actually be run,
and it should be the one the tests use.

### 3.1 What is implemented

The engine takes a `--mode` of `test`, `distributed` or `standalone`. Only
`test` runs; the other two are recognised and rejected, so that the mode axis
exists in the code rather than arriving later as a retrofit. `standalone` is a
third mode this section did not previously anticipate: the distributed server,
writing to a local directory instead of to `object-storage`.

Within `test` mode, the first steps of the batch sequence exist, plus the
reporting that says whether they worked:

- a render request is read from the YAML file named by `--request`, and its
  shape checked, before anything is loaded (§3.2);
- an empty `engine-db` is opened (in memory, per §5; `--db-path` puts it on
  disk as a debugging affordance) and the schema is applied to it from the one
  Cypher file named by `--schema`;
- an initial database state is loaded from the one Cypher file named by
  `--state`;
- with `--validate-db`, the integrity of what loaded is checked (list shapes,
  and the ranges of the columns the engine reads), and the run stops before
  precomputation if a check fails; see [integrity.md](integrity.md);
- every partial is precomputed: its `PartialSegmentTimeline` of integrable
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
[ADR 0005](../../docs/adr/0005-load-cypher-into-engine-db-in-process.md).

What the harness does **not** yet have, in the order it will need it:

- **Label selection.** A request's labels are checked for existence and
  then ignored: every partial in a frame's scope is rendered
  (rendering.md §2, §13).
- §1's responsibilities 3 and 4, and the rest of 5 and 6 -- phase
  compensation, and the features the first versions leave out
  (precomputation.md §2, rendering.md §2).
- **The production egress.** Files in a directory are the test harness's
  egress; `object-storage` is not written (rendering.md §13).

The `--state` file is also not `canonical-form`. It is Cypher applied directly
to `engine-db`, which sidesteps responsibility 2 ("load `canonical-form` into
it, and set dirty flags") rather than implementing it — and that is the step
overview.md §10 leaves open, so it could not be implemented yet in any case.

### 3.2 The test-mode render request

`--request` names one YAML file, required in test mode:

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
label set and a source version. In test mode the version is implicit: it is
whatever `--state` loaded, and there is no other to name. When requests arrive
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
persistence requirement, so an in-memory Ladybug instance is the natural fit.
Ladybug supports in-memory and on-disk with the same code, which is one of the
reasons it was chosen over TerminusDB (AGENTS.md, migration rationale).

The scaffolding keeps an on-disk path available as a debugging affordance only:
being able to open a captured `engine-db` in the `lbug` shell and inspect it
is worth the option. It is not part of the deployed configuration.

Being in-process is also the whole point of the 0.2 redesign. The transform
reads and writes the same store, so the score is never serialised in order to be
converted.

## 6. Open questions

Carried from overview.md §10, plus those specific to this component.

- **The engine schema is incomplete.** `engine-db/schema/` currently holds the
  rendering-model concepts only. The canonical-model concepts have still to be
  merged in, and `canonical-db/` — the one remaining design artefact it draws
  on — declares seven node tables that `engine-db` also declares. Deciding
  whether those become shared nodes carrying both perspectives, or stay
  separate, is the first thing that has to happen before any of this is
  implementable.
- **The rest of `engine-db`'s scope is stated but not designed.** It is meant to
  hold everything the engine needs: the canonical model, dependency information,
  precomputation output and rendering results. Nothing in the schema yet
  represents dependencies or precomputation output. See
  [ADR 0006](../../docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md).
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
  question is confined to that layer.
- ~~**The definition of an *integrable segment*** and how its boundaries are
  chosen are not specified.~~ Specified in
  [precomputation.md](precomputation.md) §6.3–6.4: boundaries are the merged
  change points of every input, and segments run between consecutive ones.
- **Phase compensation placement.** Whether it is expressed as OpenCypher against
  `engine-db` or as C++ over extracted values is not settled.
- **Job lifecycle and backpressure.** At target scale (1M+ partials) an instance
  has a great deal of concurrent work in flight. What limits it, and how partial
  failure is handled, are not designed. Idempotency makes retry safe in
  principle; the mechanism is still needed.

## 7. Boundaries this component must not cross

- **The timebase boundary — internal to this component.** Only the transform
  stage converts between tempo time and wall-clock time. Nothing in the render
  path may acquire knowledge of the tempo model.

  This used to be a component boundary, enforced by `db-transformer` being a
  separate program. It is now enforced by convention and review. Treat a
  tempo-aware type reaching the render path as a defect of the same severity as
  a correctness bug, because the architecture no longer has any way to stop it.

- **The network boundary.** Specifications and URLs cross it; audio payloads do
  not. Audio moves through `object-storage`.

- **The shard boundary.** An engine instance computes from the partials it owns.
  If rendering a frame ever requires data from another shard, either the sharding
  is wrong or the partial-independence principle has been violated.
