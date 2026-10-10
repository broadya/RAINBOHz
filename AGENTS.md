# AGENTS.md

This is the canonical machine-facing guidance for the repository. It is written
to be agent-agnostic (any coding agent, not one specific tool). `CLAUDE.md`
intentionally contains only a pointer here.

## Project Overview

This is a PhD project in computer music: "macro-scale additive synthesis." The system decouples additive synthesis partials from symbolic notes, merging composition and sound design across long time scales. Architecture is inspired by Pixar USD/Hydra.

This repo contains the production implementation. The `proof-of-concept/` directory is for general proof-of-concept work, kept outside the production components (see below).

**Ignore `lab-book/`.** It is the author's private notes, not a maintained project history. Do not read it, cite it, or treat it as a source of truth.

## Architecture

The system is an end-to-end pipeline with multiple stages. This initial phase implements the first stage: a domain-specific build pipeline that compiles a YAML-based DSL into a graph database.

### Pipeline Overview

1. User edits YAML files (the DSL source).
2. Files are committed to git (local).
3. A git hook triggers a Ninja-based build.
4. Ninja orchestrates Python build steps that transform YAML through intermediate representations into OpenCypher queries.
5. OpenCypher is executed against an embedded LadybugDB (Kuzu fork) graph database.
6. The graph DB stores both domain objects and build provenance metadata.

### Key Design Decisions

- **Build system is Ninja, not CI/CD.** Ninja handles the build DAG (stage ordering, parallelism, incremental rebuilds). Git hooks trigger Ninja. This is a build system, not a pipeline orchestrator. A Python generator script produces the `.ninja` build file.
- **LadybugDB is the graph store.** Embedded, columnar, Cypher-compatible. Chosen for embeddability, OpenCypher support, and the property graph model. It is a community fork of Kuzu (post-Kuzu archival).
- **OpenCypher is the lowest level of the DSL.** The YAML DSL compiles down to OpenCypher. In practice, only a subset of OpenCypher will be used, determined by the graph schema.
- **Multi-stage compilation.** The DSL has layers: higher-level constructs compile to intermediate representations before reaching OpenCypher. Ninja targets encode this stage DAG.
- **Phase compensation is computed in C++ by `rendering-engine`'s precomputation**, not expressed as OpenCypher and not a separate worker (decided 2026-10-03, replacing the earlier OpenCypher idea). Phase targets are met by linear `PartialPhaseCompensationSegment` ramps, whose computation needs the same `phaseAt` as rendering and a sequential running total. See [ADR 0019](docs/adr/0019-phase-targets-are-met-by-linear-compensation-segments.md).
- **Provenance lives in the graph.** Every node/edge carries metadata: `_source_file`, `_source_hash`, `_build_timestamp`. This replaces an external manifest. Incremental rebuilds query the graph for objects whose source file changed, delete them, and re-insert.
- **Content-based invalidation, not timestamp-based.** Source files are hashed. A file that is committed but unchanged (same hash) is not rebuilt.
- **The build process is co-located with the database.** The server is a domain-specific database appliance: it accepts DSL input, compiles it internally, and stores the result. External consumers query the graph via OpenCypher or an API layer. The build machinery is an internal implementation detail.
- **No feedback anywhere in the system.** Partial state is a pure function of time, with no history-dependent stepping.
- **Partials are fully independent at the canonical form level.** This is the hard architectural boundary between the DSL and the C++ renderer (a later pipeline stage, not in scope here).

### Deployment

- **ARM-native end-to-end.** Target platforms are Apple M1 (local dev) and AWS Graviton (production). Both are arm64.
- **Docker multi-arch images.** Single image runs on both platforms. Base image: `python:3.12-slim-bookworm` (not Alpine — musl causes issues with native extensions).
- **Local development is native (not containerised).** Use the container for deployment and integration testing, not daily development iteration.
- **No Kubernetes yet.** The system is a single service with an embedded DB. Containerise it; orchestrate later only when there are multiple services that need it.
- **AWS Lambda for rendering fan-out** (future phase). Compilation and graph storage are a single server; rendering distributes across serverless workers.

## Tech Stack

- **DSL source format:** YAML
- **Patterning layer (candidate):** Strudel (JavaScript port of Tidal Cycles)
- **Build system:** Ninja (with Python generator for .ninja files)
- **Build step language:** Python
- **Graph database:** LadybugDB (embedded, linked via Python bindings)
- **Query language:** OpenCypher
- **Rendering request handler:** TypeScript (Node.js)
- **Renderer (future):** C++20 with Google Highway SIMD
- **DAW integration (future):** CLAP/ARA 3.x thin client
- **Containerisation:** Docker (multi-arch arm64)
- **Cloud (future):** AWS Lambda (Graviton), ENA networking
- **Version control:** Git (GitHub, public FOSS repo)

## LadybugDB Data Model

LadybugDB (inheriting from Kuzu) uses a **structured property graph model** that is closer to a relational database with graph query ergonomics than to a traditional graph database. Understanding this shapes how the domain model maps to storage.

### Type System

- **Node tables are types.** Each `CREATE NODE TABLE` defines a strongly-typed, schema-enforced entity type with fixed, typed properties. Every node in a table conforms exactly to the schema. There is no schema-optional or ad-hoc property model.
- **Relationship tables are typed and constrained.** Each `CREATE REL TABLE` declares its `FROM` and `TO` node table types. The database enforces these constraints — you cannot create an edge between node types that the relationship table does not permit.
- **Edges are fully property-bearing.** Relationship tables carry their own typed properties, queried and filtered in Cypher exactly like node properties.
- **Multiplicity constraints.** Relationships can be declared `MANY_TO_MANY` (default), `ONE_TO_MANY`, `MANY_TO_ONE`, or `ONE_TO_ONE`. The database enforces these at write time.
- **Multiplicity is enforced per FROM/TO pair, not across the whole table.** In a relationship table that lists several pairs, each pair is counted on its own (established by experiment on Ladybug 0.19.1). `HAS_PHYSICAL_TIMELINE` depends on this: a `Partial` can hold one `FrequencyPhysicalTimeline` *and* one `PhasePhysicalTimeline`, but not two `FrequencyPhysicalTimeline`s. The same rule means a `ONE_ONE` table cannot limit a node to one predecessor across pairs; see Linked Lists below.
- **Each node has exactly one label.** Unlike Neo4j, a node belongs to exactly one node table. No multi-label nodes.

### No Class Hierarchies

- **There is no inheritance, subtyping, or polymorphism.** No `EXTENDS`, no `INHERITS`, no shared-interface queries across node table types.
- **If the same relationship type is needed between different node type pairs, separate relationship tables are required.** Each relationship table binds to specific `FROM` and `TO` node table names.
- **The impedance mismatch is ORM-like.** The domain model maps to flat tables with discriminator properties where lightweight polymorphism is needed. This is a familiar tradeoff, consciously accepted.
- **Class hierarchies and familiar naming live in the DSL, not the graph.** The DSL presents users with musical concepts (oscillators, voices, textures). The compiler resolves these into the flat, concrete types stored in Ladybug. The canonical form boundary is where abstraction ends and concrete types begin.
- **Prefer fewer, wider node tables with discriminator properties over many narrow specialised tables.** This avoids the need to UNION across multiple tables to simulate polymorphism.

### Bidirectional Traversal

- **Both directions are indexed.** LadybugDB maintains columnar CSR adjacency indexes in both forward and backward directions. Traversing from target back to source is a first-class indexed operation with no performance penalty compared to forward traversal.
- **Edge direction is semantic, not a performance constraint.** Direction encodes domain meaning (e.g. a modulator modulates a partial) and affects multiplicity constraints, but does not create a preferred query direction.
- **This is architecturally important** because the system needs both "what modulators affect this partial?" (rendering) and "what partials does this modulator affect?" (invalidation/recompilation) to be equally fast.

### Linked Lists

Timelines are linked lists: `NEXT_POINT` leads from the timeline node to its
first point (the head pair, e.g. `FrequencyPhysicalTimeline → FrequencyPhysicalTimelinePoint`)
and from each point to the next (the successor pair, e.g.
`FrequencyPhysicalTimelinePoint → FrequencyPhysicalTimelinePoint`). The table is `ONE_ONE`, but
because multiplicity is counted per pair, the database guarantees less than
that suggests:

- **Enforced:** no branches (a node has at most one successor), and no node has
  two predecessors from the *same* pair.
- **Not enforced:** a node can have one predecessor from *each* pair. That
  allows two shapes, both errors:
  - **Merged lists** — a timeline's head is a point in the middle of another
    timeline's list, so two timelines share a tail.
  - **A cycle through the head** — a list's last point links back to its first.

  A cycle that does not pass through the head is already impossible: the node
  where it closed would have two successor-pair predecessors. So every
  violation shows up as one node that is both a head and a successor.

Both are defended by how the database is built: the upstream components
(compiler, loader) construct each list fresh and never link into another. That
defence is by construction, not by constraint, and the author judges a merge
to be an error, not a feature. Consumers must not loop without limit on the
assumption that a list ends. `rendering-engine` walks lists with cycle
detection (`walkChain`). `rendering-engine --validate-db` checks every list
type in `NEXT_POINT` for both cycles and merges
(`rendering-engine/docs/integrity.md` §4).

**Decided: no point-to-timeline membership edge.** A `MANY_ONE` edge from each
point to its owning timeline would let the database enforce ownership, and so
rule out merges. It was rejected (2026-09-23) because timeline points are by
far the most numerous nodes, and the edge is overhead the system does not need
given the defence above. Do not propose it again without new grounds.

**Decided: `FrameEntry` is the index into the lists (2026-09-24).** Rendering
a frame must not walk a list from its head. Precomputation writes, per partial
per frame in scope, one `FrameEntry` node, reached from the `Frame` node by
`FRAME_ENTRY`, leading to the partial by `FRAME_ENTRY_PARTIAL` and by
`FRAME_ENTRY_PHYSICAL_TIMELINE` to the node in each list from which that frame's
rendering starts. Every step is an indexed edge from a primary-key lookup;
nothing is filtered on a property. The node carries no domain meaning and is
the place for any per-partial per-frame state a later version keeps. See
`rendering-engine/docs/precomputation.md` §6.7. This is an index, not
ownership: it does not reopen the membership-edge decision. The scope edge
`PARTIAL_IN_FRAME` (renamed from `PARTIAL_IN_FRAME_SCOPE` on 2026-09-24)
remains alongside it for now.

### Survivability

- **LadybugDB is an at-risk open-source project** (community fork of an archived project). The MIT-licensed, embedded C++ codebase can be forked and maintained if upstream dies — this is strictly better from a survivability standpoint than depending on a server-based system in an unfamiliar language (e.g. TerminusDB's Prolog/Rust). The source compiles into the application; if the project disappears, the system still works and bugs can be patched in C++.

### Full Pipeline (Beyond This Phase)

The complete system is a multi-stage pipeline. This repo initially implements stage 1 (DSL compilation to graph). Later stages include:

1. **DSL Compilation** — YAML → intermediate representations → OpenCypher → graph DB (this phase).
2. **Canonical Form Export** — → `canonical-form` (envisaged as files, one per partial set). This is the hard architectural boundary between DSL/compilation and rendering.

   **Open.** Stages 1 and 2 describe compilation writing a graph database and then exporting from it. Under `architecture/overview.md` 0.3 the only graph database is `engine-db`, hosted by `rendering-engine`, which loads `canonical-form` rather than being written to by the compiler. Whether the compiler keeps a graph database of its own, emits `canonical-form` directly, or hands Cypher to the engine to apply, is undecided and is not being addressed yet.
3. **Rendering** — sharded, not pipelined. Partial sets are the unit of sharding: each `rendering-engine` instance owns a set of partials for the whole duration of the score, holds them in its own shard of `engine-db`, and renders any requested frame for them independently (embarrassingly parallel). Rendering includes phase accumulation via analytically integrable envelope stages, spatial encoding (HOA/Ambisonics), and acoustic effects (Doppler, air absorption, fractional delays).

   Note the difference from the earlier map-reduce framing: the axis is the same, but the lifetime is not. Shards are long-lived and hold their data; a Lambda invocation is not and does not. The point of the change is that an instance already has the specification it needs and does not have to be sent one.
4. **Audio Output** — Rendered audio served to a thin CLAP/ARA 3.x DAW plugin client that requests PCM frames from a network endpoint.

### Cloud Scaling Model

- **Rendering fan-out via AWS Lambda** (or equivalent serverless). Each partial set is an independent work unit. At target scale (1M+ partials), this means thousands of concurrent Lambda invocations.
- **Feasibility is established.** Terabytes of state, thousands of concurrent Lambdas, high-bandwidth instances — expensive but within commodity cloud economics. Not exotic hardware.
- **The architecture rides commodity cloud scaling.** Graviton gets faster, Lambda gets cheaper, network bandwidth per instance climbs. The system is designed so that cost curves move in its favour over time without code changes.
- **Compute-bound, not memory-bound.** Per-Lambda memory footprint for a ~10MB partial set is trivial. The bottleneck is FLOPS, which is the curve most reliably improving on ARM.

### Networking

- **Hub/spoke topology** for rendering distribution. A large hub server (ENA-enabled, potentially 200 Gbps on network-optimised instances like `m6in.32xlarge`) aggregates results from spoke workers.
- **ENA (Elastic Network Adapter)** provides the aggregate inbound ceiling on the hub; each spoke is independently bottlenecked by its own NIC.
- **EFA (Elastic Fabric Adapter)** is a separate interface for HPC/MPI workloads — not currently in scope but noted as an option if rendering coordination patterns require it.

### Rendering Architecture (Future Phase)

- **SIMD strategy:** Structure of Arrays (SoA) layout, vectorising across partials (horizontal) rather than across samples (vertical). The canonical form must be designed for SoA from the start.
- **Target SIMD:** Google Highway for portable SIMD across ARM targets (SVE on Graviton, NEON on Apple Silicon).
- **INT64 exact accumulation** enables true delta updates — only changed partials need re-rendering, and their contribution can be subtracted/added without floating-point drift.
- **C++ renderer** is a separate component from the Python compilation pipeline. The canonical form is the interface contract between them.

### Rendering Request Flow

Rendering happens in response to playback requests. Nothing renders speculatively.

1. A client asks `playback-server` for audio between two time bounds, with a set of labels.
2. `playback-server` inspects object storage and the latest known source version. Up to date? It sums the per-shard objects and serves directly; nothing below happens.
3. On a gap, it puts an instruction on the message bus: compile and render the required audio.
4. The compilation subsystem picks that up, compiles, and puts an instruction on the bus: perform rendering.
5. `rendering-engine` picks that up. It queries the graph for partials carrying those labels, inspects dirty flags to determine what precomputation is missing, precomputes it, performs phase compensation, and renders the frames.
6. It writes multi-channel WAV objects to object storage, records the references in the graph, and puts a completion message on the bus.
7. `playback-server` sums and serves, exactly as it would have if nothing had been required.

Steps 1–7 are the current general idea, not a settled protocol. The message bus is expected to be Kafka.

Note what step 5 does **not** do: it never inspects object storage to decide whether work is needed. The graph holds object references and dirty flags, and the engine trusts them.

**Idempotency is critical.** Parallel rendering requests targeting the same partial set may trigger duplicate precomputation. This is safe because all operations on a given version of the graph are idempotent — same inputs produce same outputs. Write contention is a performance concern, not a correctness concern. This invariant must be maintained throughout design and implementation.

### TerminusDB → LadybugDB Migration Rationale

TerminusDB was originally chosen for its git-like push/pull/clone/branch semantics and built-in structural diff, which aligned with the incremental invalidation pipeline. LadybugDB was chosen instead because:

- **Embeddability.** In-process, no network hop. Critical for tight iteration during development and for embedding into C++ renderer components in later phases.
- **C++ codebase.** Aligns with the renderer stack. Maintainable if upstream dies (unlike TerminusDB's Prolog/Rust).
- **Operational autonomy.** No server dependency. The compiled-in database survives project abandonment upstream.
- **Incremental invalidation is now handled differently.** Content-hashed provenance metadata in the graph itself, rather than relying on the DB's native versioning.
- **OpenCypher portability.** Standardised query interface is transferable if the backend ever needs to change.
- **In-memory or on-disk switching.** Useful for render nodes (in-memory during a render pass, persisted between sessions) without code changes.
- **What was given up:** Git-style push/pull/clone/branch, built-in structural diff, OWL-fragment class hierarchies with property inheritance. These are either handled at other layers (DSL, provenance metadata, application code) or were not needed for the flat, concrete canonical form.

## Components

Four subsystems: compilation, rendering, playback, and orchestration.

1. **Compilation** — runs as CI/CD, triggered by a change to source.
   - **HzL (Hertz Language)** — A declarative, domain-specific language for macro-scale additive synthesis.
   - **HzL Compiler** — Python. Transforms HzL (YAML) through intermediate representations into `canonical-form`. Implemented in its first version in [`hzl-compiler/`](hzl-compiler/): it lowers the proof of concept's constructs, moved to musical time, to the canonical model as tables (`canonical-form`) and serialises them as Cypher that `rendering-engine --state` loads unchanged (ADR 0015). See `hzl-compiler/docs/design.md` and `hzl-compiler/docs/hzl-reference.md`.
   - **Build System** — Ninja-based, with a Python generator for `.ninja` files.
   - **`canonical-form`** — the lowest level of HzL: the durable representation of a score collapsed to its minimum form, in musical time and practitioner-facing units. It is not loaded directly; `rendering-engine` loads a serialisation of it to the `engine-db` data model. That serialisation is a Cypher script in the first version; at target scale it is undecided, with Parquet a strong candidate.
2. **Rendering** — one component, doing all of it.
   - **`rendering-engine`** — C++. Hosts `engine-db` in process; loads the serialised `canonical-form` into it; transforms the canonical model into the rendering model in place; determines the calculation delta from dirty flags; precomputes phase integration; performs phase compensation; renders frames to multi-channel WAV objects. Implemented in [`rendering-engine/`](rendering-engine/); see `architecture/overview.md` §5.2 and §9, where C++ is recorded as Settled. The transform (`rendering-engine/docs/transform.md`), precomputation (`rendering-engine/docs/precomputation.md`) and rendering (`rendering-engine/docs/rendering.md`) are designed and implemented in their first versions; label selection is not. It runs in two modes: `batch` (load, render a request file, exit) and `server`, a long-running REST service that holds one score in memory between calls, renders frames and answers questions about the score's tempo maps and engine parameters (`rendering-engine/docs/server.md`, `rendering-engine/api/openapi.yaml`, ADRs 0016 and 0017). The transform is the only code that reads musical time: it lives in the namespace `rainbohz::rendering_engine::transform` and the `transform/` directories, and nothing outside them includes its musical unit types or tempo map.
   - **`engine-db`** — LadybugDB (embedded), the one physical database in the system, sharded by partial set.
   - **Object storage** — WAV objects addressed by URL.
3. **Playback** — merging the shards on demand and serving them.
   - **`playback-server`** — receives client requests, sums the per-shard objects (across many `rendering-engine` instances in distributed mode), serves audio or URLs to it. Its REST interface is expected to be close to `rendering-engine`'s, with API-gateway features such as rate limiting. Not built; work in progress (`architecture/overview.md` §5.3).
   - **`playback-client`** — C++. A CLI that loops a range of the score through the default audio device, rendering every loop afresh, kept as a demonstration and a test component. Implemented in its first version in [`playback-client/`](playback-client/): until `playback-server` exists it talks to `rendering-engine`'s REST service directly, through an interface named for the API (`PlaybackApi`) so that the swap is a change of server URL; it fetches each rendered frame's `file://` object into one of two buffer lanes while the other plays. The lane protocol and the races it closes are in `playback-client/docs/design.md`; audio output is miniaudio (ADR 0018).
   - **Playback library** — C++.
   - **DAW plugin** — C++ (CLAP/ARA 3.x), wrapping the playback library.
4. **Orchestration** — the message bus gluing the other three together. Kafka is the current intention. This subsystem is at a considerably earlier stage than the rest.

One C++ component does the number crunching, co-located with its data. Python handles compilation.

An earlier revision of this document described three C++ worker types (precomputation, rendering, summing) coordinated by a C++ orchestrator, plus a TypeScript HTTP edge. That pipeline was collapsed in `architecture/overview.md` 0.2: the bandwidth cost of shipping partial specifications to remote workers exceeded the compute it offloaded. The system is still distributed, but sharded rather than pipelined — see overview.md §11.

`architecture/overview.md` is the authoritative description of the target
architecture and its component identifiers. Where this document and that one
disagree, that one wins.

### Component Directories

| Directory | Contents | Language | Status |
| --- | --- | --- | --- |
| `hzl-compiler/` | `hzl-compiler` | Python 3.12 | First version: score, tempo map, sound entity/source, envelope with `Update`, trajectory (a source's spatial position), state sets, scale, harmonic series, flattened cartesian product; frequency, amplitude and phase per partial, spatial position per source; emits Cypher |
| `engine-db/` | `engine-db` — **the implementation** | Cypher DDL | Under construction; rendering and canonical concepts, without the modulator musical timelines or dependency information yet |
| `canonical-db/` | Canonical model — **design artefact** | Cypher DDL | Loads cleanly; not the implementation; considerably out of date beside `engine-db` |
| `rendering-engine/` | `rendering-engine` | C++20 | Hosts and loads `engine-db`; transforms musical time to physical time (first version: one tempo map, frequency, amplitude, phase targets and spatial position); precomputes; renders frames to multi-channel WAV files (first version: linear curves, one spatial position per SoundSource, all eight channel modes, labels ignored); `batch` and `server` (REST) modes |
| `playback-client/` | `playback-client` | C++20 | First version: loops a range through the default audio device over `rendering-engine`'s REST service, two buffer lanes, mono; `--output-wav` for tests |

Component directory names match the canonical component identifiers from
`architecture/overview.md` §2. Do not introduce synonyms.

**`engine-db/` holds the schema of the real store.** `engine-db` is the single
physical database, hosted in process by `rendering-engine`, and
`engine-db/schema/engine-schema.cypher` is what it loads.

**`engine-db` is the engine's practical database, not one model's view of the
world.** Its scope is everything in the canonical model, plus everything needed
to determine dependencies, precompute and render: by design intent, a superset
of the canonical model. When a new concept is needed for dependency tracking,
precomputation or rendering, `engine-db` is where it goes; there is nowhere
else for it to go.

That schema is under construction: it holds the rendering-model concepts and
the canonical-model concepts, but it is not yet a full superset of the
canonical model — the modulator musical timelines are still to come — and
nothing in it yet represents dependency information. See
[ADR 0006](docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md)
and [ADR 0004](docs/adr/0004-name-the-one-database-engine-db.md).

**There is no `rendering-db/`.** It was a design artefact holding the rendering
model separately, and it was retired on 2026-09-22 once `engine-db` had absorbed
its concepts. Keeping a second schema that nothing loaded, and that duplicated
what `engine-db` already held, cost more than it explained. Older ADRs and
commits refer to it; that is history, not a directory to restore. See ADR 0006.

**`hzl-compiler/` is the compiler, and `engine-db/examples/` is the test route.**
The hand-written scores in `engine-db/examples/` are still what the engine's
own tests load. `hzl-compiler/examples/tempo-map-score/` is the HzL twin of
one of them, and `make test-hzl-oracle` proves the compiled route gives the
same audio by the existing oracle. The compiler never edits the schema and
emits only what the schema declares; a concept HzL needs that the schema lacks
(labels on sound entities, provenance columns) is the author's schema edit
first. The compiler refuses, with an error saying why, every
proof-of-concept property it does not carry (spatial position on a partial,
`pan`) rather than dropping it silently.

**`canonical-db/` is not a component, and is not loaded by anything.** It holds
the practitioner-facing model as a design artefact: useful for understanding
the problem, genuinely valid Cypher, and deliberately not what the system runs
on. It is now considerably out of date beside `engine-db`.
It is a source `engine-db` draws on. Do not treat it as the implementation, and
do not try to merge it into `engine-db` yourself — see the schema prohibition
below.

`proof-of-concept/` is for general proof-of-concept work: new ideas are tried
there, each in its own sub-directory, before being built into the production
components. No PoC is wired into any build. It once held only the frozen
initial work on RAINBOHz; those sub-directories (`audio-rendering/`,
`hzl-language/`, `maths/`) remain as a historical record that may be referenced
for design context, and are not to be modified. Current PoCs:

- `spatial-audio/` — Python 3.12 / NumPy. Virtual microphones (coincident,
  direction-only gain) for multi-channel and spatial output: mono, M/S and
  stereo, X/Y, Blumlein, FOA A- and B-format (AmbiX). It is the oracle for
  the C++20 port in `spatial-audio/cpp/`: per-channel gains for each
  `EngineConfig.channelMode` (0–7) from a position, standalone CMake with its
  own unit types and GoogleTest, checked against a golden CSV exported from
  the Python. Ported into `rendering-engine` on 2026-10-04 (`channel_mode`,
  `spatial_gains`; rendering-engine/docs/rendering.md §8.5), where the golden
  CSV is copied as `test/oracle/spatial_gains.csv`; the PoC stays the oracle.

## Graph Schema (Canonical Object Model)

The graph schema defines the canonical object model for macro-scale additive synthesis. Schema DDL is defined in `.cypher` files which serve as the source of truth.

### Schema Design Principles

- **Flat, concrete types.** Node tables represent concrete domain entities (Partial, Modulator, EnvelopeSegment, Anchor, PartialSet, etc.) with no inheritance. Each table has a fixed, enforced schema.
- **Relationship tables encode domain semantics.** Edges are typed, directional, property-bearing, and constrained to specific FROM/TO node table pairs. Use multiplicity constraints (ONE_TO_MANY, etc.) to enforce structural invariants.
- **Discriminator properties for lightweight polymorphism.** Where the domain has subtypes (e.g. frequency modulator vs. amplitude modulator), use a discriminator property (`mod_kind STRING`) on a single wider table rather than creating separate node tables per subtype.
- **Watch for UNION queries as a smell.** If queries frequently UNION across multiple node tables, the tables should probably be consolidated. The domain model has few entity types and mostly needs traversal between them — that's the sweet spot for property graphs without inheritance.
- **Build provenance on all objects.** Every node/edge carries `_source_file`, `_source_hash`, `_build_timestamp` metadata.

### Schema DDL

Schema is defined in `.cypher` files. During early development, schema migration = drop and recreate from DDL + full rebuild from source YAML.

**The `.cypher` schema files are edited by hand, by the project author, deliberately — they are the core data model.** Do not edit them, including for
trivial punctuation fixes. Validate, then report the exact line, the error, and
the corrected text, and let the author make the change. If a proposed fix needs
verifying first, test it on a copy, never on the real file.

`make validate-schemas` is the authority on whether both schemas load; run
it rather than trusting a claim here. Note what a pass does and does not
establish: it proves every statement is accepted by Ladybug. It says nothing
about whether the model is semantically correct — units, ranges, and whether a
type suits the quantity it holds are not checked by anything.

Ladybug binds relationship tables at DDL time, so a `CREATE REL TABLE` must come
after the `CREATE NODE TABLE` for every table it names. Declaration order in the
file is therefore load-bearing, and reordering is the usual fix when a rel table
reports `Table X does not exist`.

## Build

The repository build is driven by a conventional root `Makefile`, run from the
repository root. This is the **software-engineering build** (generating docs and
compiling components). It is distinct from the DSL/content compilation pipeline
described under Architecture — that pipeline is Ninja-based and not yet
implemented. Do not conflate the two.

C++ components are built by CMake. The Makefile targets are thin wrappers over
`cmake --preset`, so that one command works from the repository root whichever
component you are touching; using CMake directly is equally valid. `make help`
lists every documented target.

Documentation:

- `make docs` — regenerate the root `README.md`, all schema Mermaid diagrams
  (`.mmd`) from their `.cypher` sources, and the Doxygen API documentation. Incremental: only
  rebuilds when a source, generator script or `Doxyfile` is newer than the
  output.
- `make api-docs-rendering-engine` — Doxygen HTML for `rendering-engine` only,
  written to `rendering-engine/build/doxygen/html/index.html`. It is configured
  by `rendering-engine/Doxyfile`, which lists only the settings that differ
  from Doxygen's defaults. The output is under the gitignored build tree and
  does not depend on the preset. Tests are not documented. Warnings about
  undocumented code go to stderr; they enforce the Documentation Comments
  convention. Requires `doxygen` (override with `DOXYGEN=`).
- `make clean-docs` — remove the generated diagrams and API documentation.

A new C++ component gets its own `Doxyfile` modelled on the rendering-engine
one, an `api-docs-<component>` target, and an entry in the `docs`
prerequisites.

Schema validation:

- `make validate-schemas` — check that every schema loads into a real
  LadybugDB. Also `make validate-engine-schema` and
  `make validate-canonical-schema` for one at a time.
- Runs as the first step of `make test`: it takes seconds, needs no compiler,
  and a schema that does not load makes the rest moot.
- Each schema is replayed into a temporary database that is discarded
  afterwards. No project database is read or written.
- `make check-schema-comments` — show where the schemas' `//` comments
  would be refilled to 64 columns. `make format-schema-comments` applies
  it in place; the schemas are hand-edited, so only their author runs that.

Databases (built from the schema DDL, which is the source of truth):

- `make init-engine-db` / `make init-canonical-db` — apply the schema to
  `engine-db/db/engine.lbug` / `canonical-db/db/canonical.lbug`. Override the
  location with `ENGINE_DB=` / `CANONICAL_DB=`.
- `make reset-engine-db` / `make reset-canonical-db` — **destroy** the database
  and recreate it from schema. This is the schema-migration path during early
  development.
- Equivalent per-directory scripts: `engine-db/init_db.sh`,
  `engine-db/reset_db.sh`, and the same pair under `canonical-db/`. They take an
  optional db path, honour `$ENGINE_DB_PATH` / `$CANONICAL_DB_PATH`, and
  `reset_db.sh` prompts unless given `-y`.
- `init` is safe to repeat: declarations that already exist are reported as
  present rather than failing. It applies only *new* declarations, so it does
  not migrate existing tables — use `reset` for that.
- Database directories are gitignored. Never commit one.
- `make` / `make all` — an alias for `make docs`. Compiling a component is
  always an explicit request; `make all` must stay fast.

`rendering-engine` (C++):

- `make rendering-engine` — configure and build.
- `make test-rendering-engine` — build, then run the tests under CTest.
  That includes the rendering oracle (`rendering-engine/docs/rendering.md`
  §11.1), an independent NumPy check of the engine's output, which is
  skipped with a notice when the `python3` first on `PATH` has no NumPy.
- `make run-rendering-engine` — run the engine in batch mode, loading two
  Cypher files into an in-memory database: `RE_SCHEMA` (default
  `engine-db/schema/engine-schema.cypher`) and then `RE_STATE` (default
  `engine-db/examples/minimum-score.cypher`), precomputing every partial and
  rendering the frames of `RE_REQUEST` (default
  `rendering-engine/examples/minimum-request.yaml`) as WAV files into
  `RE_OUT_DIR` (default `rendering-engine/build/<preset>/frames`). Override
  any of them to point it elsewhere. The engine takes one file for each and
  does not read a directory; `--schema` has a built-in default, `--state` and
  `--request` are always required. Extra engine options go in `RE_ARGS`, e.g.
  `RE_ARGS=--validate-db` to run the `engine-db` integrity checks
  (`rendering-engine/docs/integrity.md`), or `RE_ARGS=--per-set-wav` for a
  file per partial set as well as per frame.
- `make run-rendering-engine-server` — run the engine in server mode on
  `RE_HOST`:`RE_PORT` (default `127.0.0.1:8080`), reading scores from beneath
  `RE_INPUT_ROOT` (default the repository root) and writing frames to
  `RE_OUT_DIR`, until `POST /v1/shutdown`. The engine itself has no defaults for
  these; the Makefile supplies them. See `rendering-engine/docs/server.md`.
  The calls are specified by the OpenAPI 3.1.1 document
  `rendering-engine/api/openapi.yaml`, following the conventions of ADR 0017
  (resources under `/v1`, problem+json errors). Change it with the code: CTest
  `server.openapi` validates it, and the `server.*` tests check live responses
  against it, when PyYAML, jsonschema and openapi-spec-validator are installed.
  A Postman collection of the same calls,
  `rendering-engine/test/postman/rendering-engine.postman_collection.json`,
  is generated from the document by `make_postman_collection.py` beside it;
  regenerate it when the document changes, and never edit the JSON by hand.
  The server includes its own cpp-httplib under a renamed namespace because
  Ladybug links an older copy; see ADR 0016 before bumping either.
- `make format-rendering-engine` — apply `.clang-format` in place.
- `make tidy-rendering-engine` — run `clang-tidy`.
- `make clean-rendering-engine` — remove the build tree.

`playback-client` (C++), built with the same presets and conventions:

- `make playback-client` — configure and build. `PC_PRESET` defaults to
  `RE_PRESET`.
- `make test-playback-client` — build the client and `rendering-engine`, then
  run the client's tests under CTest: GoogleTest unit tests that drive the
  two threads over a scripted fake API, and `playback.e2e`, which plays three
  loops from a real server to `--output-wav` and checks every sample against
  the server's frames. The end-to-end test is skipped when the engine is not
  built for the same preset.
- `make run-playback-client` — play `PC_START` to `PC_END` (default `0:0` to
  `4:0`), `PC_LOOPS` times, from the server at `PC_SERVER` (default the
  address of `make run-rendering-engine-server`, with `/v1`). The client does
  not load a score: `PUT /v1/score` first. Extra options go in `PC_ARGS`,
  e.g. `PC_ARGS="--debug --keep-buffer"`.
- `make format-playback-client`, `make tidy-playback-client` (currently
  clean), `make api-docs-playback-client`, `make clean-playback-client` — as
  for the engine. The component has its own `.clang-format`, `.clang-tidy`
  and `.clangd`, copied from the engine's.

`hzl-compiler` (Python):

- `make hzl-compiler` — create `hzl-compiler/.venv` from `PYTHON` (3.12) and
  install the package into it, editable, with its test dependencies. Once.
- `make hzl-compile` — compile `HZL_SCORE` (default the tempo-map twin) to
  `HZL_OUT` (default `hzl-compiler/build/<score>.cypher`). Extra `hzlc`
  options go in `HZL_ARGS`.
- `make run-hzl-score` — compile, then render with `RE_REQUEST` into
  `RE_OUT_DIR`, exactly as `run-rendering-engine` would from a hand-written
  state file.
- `make test-hzl-compiler` — pytest.
- `make lint-hzl-compiler` — ruff lint and format check; changes nothing,
  fails on any finding. Part of `make test`.
- `make format-hzl-compiler` — apply ruff's safe fixes, then `ruff format`,
  in place.
- `make test-hzl-oracle` — compile the twin, render it, and check every frame
  against `engine-db/examples/tempo-map-score.json` with the rendering
  oracle. Needs the engine built and NumPy on `python3`.
- `make clean-hzl-compiler` — remove `hzl-compiler/build` and the venv.

Aggregates: `make build`, `make test`, `make clean`. `make test` runs the
compiler's lint and tests before the engine's, then the playback client's, and the
oracle check after.

The build configuration is selected with `RE_PRESET`, which defaults to
`macos-debug` on Darwin and `linux-debug` elsewhere:

```
make rendering-engine RE_PRESET=macos-release
```

**The first C++ build compiles Ladybug from source and takes a long time.**
Later builds are incremental and ccache-accelerated. Build trees are per-preset
(`rendering-engine/build/<preset>/`), so switching configuration does not
force a rebuild of the other one.

Generated diagrams are **pure Mermaid** (no Markdown fence — wrap at the point
of use if embedding in `.md`):

| Source schema | Generated diagram |
| --- | --- |
| `engine-db/schema/engine-schema.cypher` | `engine-db/docs/engine-schema.mmd` |
| `canonical-db/schema/canonical-schema.cypher` | `canonical-db/docs/canonical-schema.mmd` |

To add a schema, add a target rule and an entry in the Makefile's `DIAGRAMS`
list.

## Conventions

- Schema changes are DDL files, not ad-hoc queries.
- During early development, schema migration = drop and recreate from DDL + full rebuild from source YAML.
- Architecture Decision Records (ADRs) in `docs/adr/` for non-obvious design choices, following the [MADR](https://adr.github.io/madr/) convention. Start from `docs/adr/adr-template.md`; see `docs/adr/README.md` for numbering and status rules. ADRs record the current situation and are edited in place when it changes; git holds the history. A decision replaced outright is marked superseded by its successor.
- `proof-of-concept/` is for general proof-of-concept work; no PoC is added to any build. Its original sub-directories (`audio-rendering/`, `hzl-language/`, `maths/`) are a historical record: do not modify them.
- The project is licensed Apache-2.0 (root `LICENSE`). Every first-party source and build file (`.cpp`, `.hpp`, `.py`, `.cmake`, `.sh`, `CMakeLists.txt`, the root `Makefile`) begins with `SPDX-License-Identifier: Apache-2.0` as a comment in the file's own syntax, followed by a blank line; after the `#!` line in a script, and before `#pragma once` in a header. Only the license line, no copyright line. A new file gets it too. Not applied to the frozen PoC directories, generated files, data, Markdown, or the `.cypher` schemas.
- No part of this repository is published to PyPI or any other package registry. Every `pyproject.toml` carries `classifiers = ["Private :: Do Not Upload"]`, which PyPI rejects on upload; a new one gets it too. Installing dependencies into a local venv is fine.
- `CHANGELOG.md` ([Keep a Changelog](https://keepachangelog.com/en/1.1.0/)) is AI-maintained. A change a reader of the project would notice (a feature, behaviour, interface, schema, licence or removal) adds a line under `## [Unreleased]` in the same commit. Internal refactors, test-only and formatting changes do not. On a release, the Unreleased entries move under the new version's heading with its date.

### Documentation Ownership

The root `README.md` is generated by `make readme` (part of `make docs`), which
joins two parts in `docs/readme/`. Never edit `README.md` directly; edit the
part, then run `make readme`, and commit both.

- `docs/readme/introduction.md` — everything before `## Repository Layout`:
  the project summary, AI disclosure, architecture, languages, platforms and
  status. **This is the author's own writing. AI agents must not modify it in
  any way** — not to fix typos, update facts, reformat or reword. If it looks
  wrong or out of date, say so to the author and leave it unchanged.
- `docs/readme/developer-guide.md` — `## Repository Layout` onwards. Agents
  maintain this part and keep it current as the repository changes.

### C++ Conventions

These apply to `rendering-engine` and to C++ components added later.

- C++20, per README.md. Ladybug is C++20; going higher buys nothing until a C++23 feature is actually needed and would narrow the usable compiler set on the Linux and Windows targets. For parallelism and SIMD the trigger to revisit is C++26 (`std::simd`, `std::execution`) once it ships in all three toolchains; until then SIMD comes from Highway, which needs only C++17.
- C++ Core Guidelines, enforced as far as the `cppcoreguidelines-*` set in `.clang-tidy` reaches. `make tidy-rendering-engine` and `make tidy-playback-client` are currently clean; keep them that way.
- Types `PascalCase`, functions and variables `camelCase`, private members trailing `_`, constants `kPascalCase`. Encoded in `.clang-tidy` as `readability-identifier-naming` options.
- **Physical quantities and identifiers get their own unit types**, never
  bare primitives and never plain aliases. A time in samples, a phase, a
  frequency, a frame number and a frame size are each a distinct type: a
  one-member struct, explicitly constructed, read back with `.value()`, with
  no implicit conversion to or from its representation or any other unit type.
  A `using` alias is not enough, because the compiler treats it as its
  underlying type and would still accept a frame size where a frame number
  belongs (Core Guidelines I.4, P.1).
  - Define only the operations that make physical sense, so that unit errors
    fail to compile. Absolute time and duration are separate types, as with
    `std::chrono`'s `time_point` and `duration`: time − time is a duration,
    and time + time does not compile. A cross-unit product such as
    frequency × duration yields the correct result type (phase).
  - Where mixed-unit arithmetic has no natural result type, unwrap with
    `.value()`, compute, and wrap the result. Keep that code in the maths
    modules, where it is easy to find and review, not scattered through callers.
  - Wrap and unwrap at boundaries. Values from `engine-db` are wrapped as soon
    as they are read, and unwrapped only to be written. Ladybug never sees a
    unit type. SIMD kernels work on raw Structure-of-Arrays arrays: unit types
    belong at function interfaces, not inside vectorised inner loops.
  - Unit types must cost nothing at run time: trivially copyable, with inline
    operators.
  - The current set of types and their operations is in
    [rendering-engine/docs/precomputation.md](rendering-engine/docs/precomputation.md)
    §3.1, extended by
    [rendering-engine/docs/rendering.md](rendering-engine/docs/rendering.md) §3.
- **Write for the parallel version, implement the serial one.** Rendering
  will be optimised for parallel execution (SIMD across partials, and threads
  across partial sets) in a later phase. Until then the focus is correctness,
  but code is placed so that the transition does not require restructuring:
  - Keep the per-sample, per-partial work in its own small kernel functions
    over Structure-of-Arrays data, with no database access, allocation,
    locking, exceptions or I/O reachable from them.
  - Separate the three layers explicitly: gathering (database to in-memory
    structures), preparation (per-frame, per-partial-set state), and the hot
    loop. Only the first touches `engine-db`.
  - Partial sets are the unit of parallelism between threads and shards; a
    partial set's rendering must not depend on any other's.
  - Do not introduce feedback or history between partials, and do not let a
    per-partial branch that a later SIMD version could not take hide inside
    the hot loop when it can be lifted out (for example, by splitting a frame
    at the times where any partial changes state).
  This is a standing constraint on every rendering change, not only the first
  version. See "Rendering Architecture" above for the SIMD strategy.
- Formatting is `.clang-format`, kept close to Ladybug's own so that reading between the two codebases is not jarring. Apply with `make format-rendering-engine`.
- This is not the Google C++ Style Guide, and its naming, formatting and file-extension rules do not apply. Where the two conflict, the Core Guidelines win: in particular, errors are reported with exceptions (Core Guidelines E.2), not Google-style status returns.
- `[[nodiscard]]` only where discarding the result is a real hazard: a failure reported by return value (`tryExecute`, `loadCypherFile`), or a resource that would leak. Not on ordinary pure functions or accessors, where it is noise. This follows the standard library's own policy (P0600).
- Sources are listed explicitly in CMake, never globbed. Globbing hides added files from the configure step.
- Third-party headers are included as SYSTEM headers, so that our warning set does not fire on code we do not own.
- Third-party dependencies are built from pinned source via `FetchContent`, never taken from the system. This is what keeps cross-compilation possible; see ADR 0001.
- Component-owned public headers must not expose third-party types. Callers should not acquire a transitive dependency on Ladybug's API by including ours.
- Every function is documented to the Documentation Comments convention below.

### Python Conventions

These apply to `hzl-compiler` and to Python components added later.

- Python 3.12, with type hints on every function.
- [ruff](https://docs.astral.sh/ruff/) is the linter and formatter,
  configured in the component's `pyproject.toml`: 100-column lines, rule
  groups E, W, F, I, UP, B and N. `make lint-hzl-compiler` is clean; keep it
  that way. Run `make format-hzl-compiler` after editing, and fix by hand
  what it cannot (usually a long string: split it into adjacent literals).
- Suppress a finding only with a `# noqa: <code>` on the line and a reason,
  or with a config entry that says why.

### Documentation Comments

Every function has a comment block immediately before it that explains it.
This applies to all languages in the repository except the original, historical sub-directories of `proof-of-concept/`. The block
can be short when the function is simple, but it must cover:

1. **Purpose.** The first line is a one-sentence summary of what the function
   does. Rationale, context and "why" come in the paragraphs after it.
2. **Every parameter.** State its meaning, constraints and units, not its type,
   which the signature already gives.
3. **The return value**, if there is one. Say what the value means.
4. **Modifications and side effects.** Say what the function changes in an
   argument passed by non-const reference or pointer. Also cover changes the
   caller cannot see in the signature: database writes, file I/O, member or
   global state, and output to stdout/stderr.
5. **Exception conditions.** List each exception the function throws, or
   deliberately lets through for the caller to handle, and the condition that
   causes it. Exceptions that any code can raise (`std::bad_alloc`) are not
   listed.

Rules that go with it:

- **Units are mandatory for physical quantities.** Frequency in Hz, phase in
  radians or cycles, time in seconds or samples, amplitude linear or dB. An
  unstated unit is a bug waiting to happen, not just vagueness. In C++, a
  parameter whose type is a unit type (see C++ Conventions) already states its
  unit; the comment then covers its meaning and constraints, such as whether a
  time is absolute or local to a segment.
- **The short form**, a one-line summary and nothing else, is allowed only when
  every other item would say "none": no parameters, or only ones whose meaning
  is obvious from their names; no modifications; no exceptions. So a missing tag
  always means "nothing to say", never "not written".
- **Include these where they apply**; they are not required on every function:
  preconditions and postconditions (Core Guidelines I.5, I.7); ownership and
  lifetime (does the function keep a reference or pointer beyond the call?);
  real-time safety for anything reachable from a render or audio path (does it
  allocate, lock, block or throw?); thread safety; complexity on hot paths; and
  cross-references to ADRs and design docs.
- **Exempt:** overrides, which inherit the base class documentation (Doxygen
  `INHERIT_DOCS`); defaulted and deleted special members; lambdas; and test
  cases, where the test name is the documentation.
- **Explain, don't restate.** A comment that repeats the signature adds nothing
  and goes stale. Keep documenting the reasoning, as the existing comments do.

**C++** uses Doxygen `///` comments with `@` commands:

- Public functions are documented on the declaration in the header. Internal
  functions (anonymous namespace, `static`, private helpers) are documented on
  the definition. Never both: two copies drift apart.
- `@param name` for each parameter. Mark a parameter the function modifies
  with `@param[in,out]` or `@param[out]` and say what changes. Parameters
  passed by value or `const&` take no direction.
- `@return` for every non-`void` function.
- `@throws Type` for each exception, with its condition. A function that throws
  nothing is better marked `noexcept` than described in prose.
- `@pre`, `@post`, `@note` and `@see` where they apply.

```cpp
/// Applies every statement in a Cypher script to `db`, in source order.
///
/// Order within the file is load-bearing: Ladybug binds relationship tables at
/// DDL time, so a CREATE REL TABLE must follow the CREATE NODE TABLE for every
/// table it names. This function does not reorder anything.
///
/// @param[in,out] db  Database the statements run against. Every statement
///                    that succeeds is committed to it.
/// @param file        Script to apply. Must be a readable regular file.
/// @param observe     Called once per statement with its outcome, in order.
///                    Optional; empty means no callback.
/// @return Counts of applied and already-present statements, plus one
///         ScriptFailure per rejected statement. Rejections are reported
///         here, not thrown.
/// @throws ScriptLoadError if `file` is missing, not a regular file, or
///         unreadable. Nothing has been executed against `db` at that point.
[[nodiscard]] LoadReport loadCypherFile(
    EngineDb& db, const std::filesystem::path& file, const StatementObserver& observe = {});

/// True when no statement failed.
bool ok() const noexcept;
```

**Python** uses Google-style docstrings (`Args:`, `Returns:`, `Raises:`), the
usual Python idiom. List modified arguments under `Args:` with what changes,
and other side effects in the body. Python is not currently run through
Doxygen.

```python
def run_statement(lbug: str, db_path: str, statement: str) -> Result:
    """Execute one statement against a database in its own lbug process.

    Args:
        lbug: Path to the lbug CLI.
        db_path: Database directory. Modified: the statement is committed
            to it if it succeeds.
        statement: One Cypher statement, without its terminating semicolon.

    Returns:
        Success, or failure with lbug's error text. A timeout (120 s) or a
        non-zero exit is reported as a failure here, not raised.

    Raises:
        FileNotFoundError: if `lbug` is not an executable.
    """
```

**Enforcement:**

- Doxygen (`make docs`) reports every undocumented function, parameter or
  return value as a warning (`WARN_IF_UNDOCUMENTED`, `WARN_NO_PARAMDOC`).
  Internal functions are extracted as well, so they are checked too. The
  warnings do not yet fail the build. Once the existing code conforms,
  `WARN_AS_ERROR` in the component's `Doxyfile` becomes `FAIL_ON_WARNINGS`.
- clang's `-Wdocumentation` is part of the rendering-engine warning set. It
  catches documentation that no longer matches the code: a `@param` naming a
  parameter that doesn't exist, or a `@return` on a `void` function. It does
  not report missing comments.

## Developer Tools

### C++ Toolchain

Required for the C++ components. On macOS, via Homebrew:

| Tool | Purpose | Notes |
| --- | --- | --- |
| Xcode Command Line Tools | clang, lldb, SDK | `xcode-select --install` |
| `cmake` | build configuration | ≥ 3.28 |
| `ninja` | build execution | the generator every preset uses |
| `ccache` | compile caching | detected automatically by CMake |
| `llvm` | `clang-tidy`, current `clang-format`, `lld` | keg-only; not on `PATH` |
| `doxygen` | API documentation | needed by `make docs`; ≥ 1.9.5 for `WARN_IF_INCOMPLETE_DOC` |

Homebrew's `llvm` is deliberately not linked into `PATH`, so it cannot shadow
Apple clang. It is used for two things only: `clang-tidy`, which Apple clang
does not ship at all, and cross-compilation, where Apple clang cannot target
Linux. The Makefile finds it at `/opt/homebrew/opt/llvm/bin`.

The compiler is pinned to `clang`/`clang++` in `CMakePresets.json` rather than
taken from the environment. A stale `CC`/`CXX` export must not silently change
what the project is built with.

Ladybug itself is **not** a system dependency of the build — it is compiled
from pinned source (see `docs/adr/0001-build-ladybug-from-pinned-source.md`),
taken from our fork, which carries patches upstream does not have yet (see
`docs/adr/0010-build-ladybug-from-our-fork.md`).
The Homebrew `ladybug` package is still worth having as a shell for inspecting
databases by hand, but it is not what the components link against, and the two
versions may differ.

### Schema Visualisation

`scripts/ladybug_schema_to_mermaid.py` — converts one or more LadybugDB DDL `.cypher` files into a Mermaid UML class diagram. REL tables are rendered as association classes (junction-table style) between the node table classes they connect. Multiplicity constraints are shown as cardinality labels on the association lines.

```
python3 scripts/ladybug_schema_to_mermaid.py canonical-db/schema/canonical-schema.cypher
```

By default it emits raw Mermaid on stdout. An optional `--markdown` flag wraps
the output in a fenced ```mermaid block. In this repository the diagrams are
generated via `make docs` (see Build), which writes pure `.mmd` files — prefer
that over invoking the tool by hand.

### Schema Validation

`scripts/validate_ladybug_schema.py` — validates DDL `.cypher`
files by **executing them against a real, throwaway LadybugDB**, so that
"valid" means what the database actually accepts rather than what a
reimplemented parser believes. Requires the Homebrew `lbug` CLI. Normally
invoked via `make validate-schemas` (see Build).

```
python3 scripts/validate_ladybug_schema.py \
    engine-db/schema/engine-schema.cypher --verbose
```

It reports each failing statement with its source line range and the exact
database error, then a count of the tables that actually got created.

**`lbug` cannot be trusted as a batch loader.** Three behaviours of 0.19.1,
each established by experiment, drive the design of both the validator and the
loader — and any future code that drives this CLI has to account for all three:

1. It reports failures on **stdout**, not stderr.
2. It exits **0** even when statements fail.
3. Fed a whole schema file on stdin, it **silently skips** some statements —
   they neither execute nor report an error. The same statement succeeds when
   submitted on its own. Piping a whole schema in creates fewer tables than
   submitting its statements individually.

(3) is the dangerous one: a schema can half-load with no diagnostic at all.
So DDL is submitted one statement per `lbug` process against a persistent
database (dependencies between statements still resolve), and failure is
detected by scanning output for error markers rather than by exit code. The
cost is one process per statement, a few seconds for a whole schema.

**Never pipe a `.cypher` file straight into `lbug`.** Use
`scripts/load_schema.py`, or the `make init-*-db` targets that wrap it.

This is not hypothetical: the original `canonical-db/init_db.sh` piped whole
files in and captured only stderr, so it reported success on half-loaded
databases. That is how six broken `STRUCT` declarations survived in the
canonical schema undetected.

### Shared DDL machinery

`scripts/ladybug_ddl.py` holds the statement splitter, the executor and the
error detection, and is the single home for the subtleties above. Both
`scripts/load_schema.py` (the loader) and
`scripts/validate_ladybug_schema.py` (the validator) import it.
Fix a parsing or error-detection bug there and both benefit.

The validator, the diagram generator and the comment wrapper all live in
`scripts/` beside it, so everything `make` runs is in the repository. `tools/`
is gitignored and holds only tooling outside the build (the Ladybug Cypher VS
Code extension, destined for a separate repository).

The reasoning, the options weighed, and the conditions under which this should
be revisited are recorded in
[ADR 0002](docs/adr/0002-apply-ddl-one-statement-per-process.md).

## Domain Context

- Modulation model is multiplicative (frequency modulators compose multiplicatively; amplitude modulators multiply in linear domain).
- Two-layer architecture: master timeline defines partial existence and core parameters; modulators are multiplicative perturbations selected by label-based filtering.
- "Sound" is the DSL-facing term; "Sound Event" is the compiled canonical form.
- Canonical form is multiple files, one per partial set.
- Rendering (a later pipeline stage) is an HPC map-reduce pipeline targeting ARM/Graviton with Google Highway SIMD and INT64 exact accumulation.