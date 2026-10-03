# RAINBOHz Reference Implementation — Architecture Overview

**Status:** working draft
**Describes:** the target architecture. This document does **not** describe the current state of the code.
**Version:** 0.5, 2026-10-03

**Changed in 0.5.** `playback-server` is described more fully: besides summing across the
`rendering-engine` instances, it presents a REST interface close to `rendering-engine`'s, carries
API-gateway features such as rate limiting, and serves audio or URLs to it. The playback sequence
diagram moved here from `playback/`, as `05-playback-sequence.mmd`.

**Changed in 0.4.** `rendering-db/` is retired. `engine-db` is not one of two models any more: it is
the engine's whole database, holding everything in the canonical model plus everything needed to
determine dependencies, precompute and render. The rendering model was never a separate store — it
was a separate *description* of one — and keeping a schema that nothing loaded cost more than it
explained. `canonical-db/` is retained as the one remaining design artefact. See
[ADR 0006](../docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md).

**Changed in 0.3.** The one physical database is named `engine-db`, superseding the name
`synthesis-db` used in 0.2. Its schema now exists, under construction, in `engine-db/schema/`: it
starts from the rendering-model concepts and absorbs the canonical ones incrementally. `canonical-db/`
and `rendering-db/` are retained as design artefacts (`rendering-db/` since retired; see 0.4). See
[ADR 0004](../docs/adr/0004-name-the-one-database-engine-db.md).

**Changed in 0.2.** The rendering pipeline collapsed into a single component. `db-transformer`,
`precompute-worker` and `render-worker` no longer exist; `render-orchestrator` became
`rendering-engine` and absorbed their work. The two graph databases became one. `audio-service` and
`summing-worker` became `playback-server`. See §11 for why.

---

## 1. Purpose and scope

This document is the compact, top-level description of the RAINBOHz reference implementation for
macro-scale additive synthesis. It covers the whole system: HzL source code, compilation, the
database, distributed rendering, audio delivery, and the client applications.

It exists to serve two audiences:

1. Human readers who need the shape of the system in one sitting.
2. AI agents that will be given this document as context when working on individual components.

Its deliberate limits:

- It is **shallow**. Each component gets a responsibility statement and its relationships to other
  components, and nothing more. Component-level detail belongs in a separate document per component.
- It is **early**. Many end-to-end questions are unresolved. Section 10 records the open
  ones that are known; it is not exhaustive.
- It is **target state**. Nothing here should be read as a claim about what is implemented.

---

## 2. Conventions

**Component identifiers.** Every component has one canonical identifier in `kebab-case`. That
identifier is used verbatim as the section anchor in this document, as the node identifier in the
Mermaid diagrams, and in all prose. `grep -r rendering-engine .` finds every reference to that
component. Do not introduce synonyms or prose variants.

**Diagrams.** Mermaid diagrams live in separate `.mmd` files alongside this document in
`architecture/`, one file per diagram. They are numbered to match the order in which they are
referenced here. The diagrams are a **redundant second view**: the prose in this document carries the
complete information content on its own, and a reader who cannot open the `.mmd` files loses nothing
but convenience.

**Time bases.** Two distinct time bases exist in this system and are never conflated. *Musical time* is
measured in musical units against a polytempo tempo model, and moves when the tempo changes.
*Physical time* is measured in seconds or samples, and does not depend on tempo. The boundary
between them lies inside `rendering-engine`; see §8. In `engine-db` every timeline says which it is
in its name (`…MusicalTimeline`, `…PhysicalTimeline`); see
[ADR 0012](../docs/adr/0012-distinguish-musical-and-physical-time.md).

**Models and stores.** The *canonical model* and the *rendering model* are two conceptual models.
They are not two databases. There is one physical store, `engine-db`. Keep the distinction in
mind when reading: a claim about a *model* is a claim about shape and units, not about where bytes
live.

---

## 3. Design principles

These principles constrain every component below. Where a principle is phrased as a prohibition, the
prohibition is load-bearing: conventional synthesis architectures violate it, and an implementation
that drifts back toward the conventional design is wrong even if it sounds correct.

- **Macro time scale.** Partials are specified and manipulated at the time scale of musical
  structure, not at the time scale of individual audio events.
- **Partial independence.** After compilation, partials are independent of one another. A partial's
  contribution is computable without reference to any other partial.
- **No feedback.** No component introduces a feedback path. Audio-rate state does not loop back into
  its own input anywhere in the system.
- **Random access.** Rendering any region of the score does **not** require rendering the regions
  that precede it. Any frame is renderable in isolation, given its partial specifications and
  precomputed phase. This is what makes sharding possible at all: a shard that holds some partials
  for all time can render any frame without talking to any other shard.
- **Two-model separation.** The composer-facing model and the render-facing model are separate
  representations with separate timebases and separate units. Rendering concerns do not leak into the
  composer-facing model; composer-facing units and time do not survive into the render path. This is
  a separation of *models*, not of *stores* — see the next principle.
- **Locality of reference.** Computation lives where its data lives. `rendering-engine` holds the
  graph and performs the transformation, precomputation and rendering over it, because moving
  specifications to a remote worker costs more than the work it offloads.
- **Sharding, not pipelining.** The system is distributed, and scales by partitioning the partial
  space across identical engine instances — not by decomposing the render into specialised stages
  connected by the network. Every instance can do every job for its own shard. A network hop inside
  what could be one process is a design failure, not a scaling strategy.

---

## 4. System context

See `01-system-context.mmd`.

The system has three external parties:

- **Composer.** Authors and edits HzL source, and auditions the result. The only human role in the
  architecture.
- **DAW host.** Supplies transport position and timeline context, and consumes rendered audio.
  Interaction is expected to be via ARA. This relationship exists only through `playback-plugin`.
- **Compute and storage infrastructure.** The deployment target for the distributed components and
  for object storage. It is outside the system boundary: the sharding and distribution design is
  architecture, the machines it runs on are deployment.

---

## 5. Component overview

See `02-component-overview.mmd`.

Ten components in four subsystems: compilation, rendering, playback, and orchestration.

### 5.1 Compilation

This subsystem is a build pipeline, and is expected to run as CI/CD: triggered by a change to
source, producing an artefact.

#### `hzl-source`
The HzL source code authored by the composer. The only human-edited artefact in the system.

#### `hzl-compiler`
Compiles `hzl-source` into `canonical-form`. Python, in its first version (`hzl-compiler/`,
2026-10-02), which lowers the proof of concept's constructs, moved to musical time, to the canonical
model as tables and emits them as Cypher (ADR 0015).

HzL is a multi-level language with several levels of abstraction. The compiler's job is to lower
higher-level constructs toward lower-level ones. The design of HzL itself is at a very early stage.

#### `canonical-form`
The compiled, loadable representation of a score, expressed in the canonical model: musical time,
composer-facing units, polytempo. This is the durable artefact of composition and the handover point
between compilation and rendering.

It is not a database. `rendering-engine` loads it into `engine-db`. What exactly it consists of —
files per partial set, a Cypher stream, something else — is **not decided**; see §10.

### 5.2 Rendering

#### `engine-db`
A Ladybug graph database, hosted in-process by `rendering-engine`. There is exactly one physical
database in this system, and this is it.

It is the engine's whole database. Its scope is everything in the canonical model as loaded, plus
everything the engine needs in order to work: the rendering model derived from the canonical one as a
second perspective over a shared set of nodes, the dependency information that drives dirty flags,
intermediate precomputation and rendering results, and references to the rendered objects in
`object-storage`.

There is no second database and no transfer between two of them. Data is sharded early and everything
belonging to a shard is held in one place — the principle of locality, and, separately, what the
bandwidth arithmetic in §11 forces. A model that lives in the same store as the data it describes does
not have to be shipped anywhere to be used.

In the distributed deployment it is **sharded by partial set**: each engine instance holds the
partials it owns, for the whole duration of the score.

Its schema is `engine-db/schema/engine-schema.cypher`, and it is **under construction**: it currently
holds the rendering-model concepts, and the canonical-model concepts are still to be merged into it.
See [ADR 0004](../docs/adr/0004-name-the-one-database-engine-db.md).

**The schema under `canonical-db/schema/` is a design artefact.** It describes the composer-facing
model and is genuinely useful for understanding the problem, but it is not loaded by this database or
anything else. It declares seven node tables that `engine-db` also declares, and those seven are the
merge question `engine-db` has still to answer; see §10.

`rendering-db/` held the rendering model the same way until 0.4, when it was retired: `engine-db`
already held those concepts, so the second copy described nothing the real schema did not. See
[ADR 0006](../docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md).

#### `rendering-engine`
A C++ program. It is the only component that touches a graph database, and it performs every
operation that needs one:

- **Hosts** `engine-db` and encapsulates all access to it.
- **Loads** `canonical-form` into it. The design of this step is open; see §10.
- **Transforms** the canonical model into the rendering model, in place. Musical time becomes
  physical time, Hertz becomes radians per sample, and the spatial-audio timing work is done here.
- **Determines the calculation delta** for an incoming request: what, if anything, actually needs
  computing.
- **Precomputes** — phase integration over integrable segments — and performs phase compensation.
- **Renders** frames to multi-channel WAV objects and writes them to `object-storage`.

It consumes render requests from `message-bus` and announces completion on it. A request names a
frame, a label set, and a source version.

**Delta determination does not inspect `object-storage`.** The engine trusts the object references
held in the graph, and computes the delta from dirty flags that the load step sets from dependency
information already in the graph. This is a load-bearing assumption: if the graph and the object
store disagree, the graph wins, and the system will serve stale audio rather than notice.

#### `object-storage`
The shared store for WAV objects. Objects are addressed by URL, and URLs rather than audio payloads
are what travel between components.

### 5.3 Playback

#### `playback-server`
Delivers audio to clients. It receives a request for audio over a time range with a label selection,
checks `object-storage` and the latest known source version, and either serves what is already there
or asks for the missing work via `message-bus`.

It also performs **summing**: merging the per-shard objects into the audio the client asked for.
In distributed mode that means summing across many `rendering-engine` instances, one per shard.
Summing is expected to be a tree, and the tree is not expected to be deep.

Its REST interface is expected to be very close to `rendering-engine`'s, so that a client written
against one needs little change to use the other. It is also where the API-gateway concerns live,
such as rate limiting. It serves either the audio itself or URLs to it, which `object-storage` then
serves; which of the two is open. All of this is work in progress.

#### `playback-client`
A simple client of `playback-server` for use outside a DAW, and during development.

#### `playback-plugin`
A client of `playback-server` that can synchronise with a DAW for playback timing. CLAP and VST are
the target plugin formats. It is expected to work via ARA. Many questions here remain open and are
not expected to be resolved soon.

### 5.4 Orchestration

#### `message-bus`
Carries the instructions that connect the other three subsystems: compile this, render this,
rendering is complete. Kafka is the current intention.

This subsystem is at a considerably earlier stage than the rest of this document. Treat §7 as a
sketch of intent rather than a specification.

---

## 6. Compilation pipeline

See `03-compilation-pipeline.mmd`.

`hzl-compiler` reads `hzl-source` and lowers its higher abstraction levels toward its lowest,
producing `canonical-form`: a representation in musical time and composer-facing units, supporting
polytempo.

The transformation to the rendering model is **not** part of this pipeline. It happens inside
`rendering-engine`, against a database that is already loaded. This is the change that 0.2 makes to
the old two-stage compilation model, and it is the point of the whole redesign: the transformation
reads and writes the same store, so it never serialises the score to move it.

`canonical-form` is durable and versioned. `engine-db` is derived from it.

---

## 7. Render and delivery pipeline

See `04-render-pipeline.mmd`, and `05-playback-sequence.mmd` for a first sketch of the same flow from
`playback-client`'s side. That sketch predates the current design in places, and its header says
where.

Rendering is performed in response to playback requests. That has always been a core concept in this
system: nothing renders speculatively.

1. `playback-client` asks `playback-server` for audio between two time bounds, with a set of labels.
   (Possibly more — multi-channel format, for instance — but that is for another day.)
2. `playback-server` inspects `object-storage` and the latest known source version. If everything is
   up to date, it sums and serves directly, and nothing below happens.
3. On a gap, it puts an instruction on `message-bus`: compile and render the required audio.
4. The compilation subsystem picks that up and compiles, then puts an instruction on `message-bus`:
   perform rendering.
5. `rendering-engine` picks that up and renders — computing the delta, precomputing what is missing,
   rendering the frames — and writes the objects to `object-storage`.
6. It puts a message on `message-bus`: rendering complete.
7. `playback-server` sums and serves, exactly as it would have if nothing had been required.

Steps 3–6 are the current general idea, not a settled protocol.

---

## 8. Key architectural boundaries

Two boundaries between components carry most of the architectural weight, and one boundary that used
to be between components now runs inside one. Changes that cross any of them are expensive; changes
within them are cheap.

**The `canonical-form` boundary.** Everything upstream is language and authoring. Everything
downstream is rendering. `canonical-form` is the only interface between them, and its definition is
therefore the most consequential contract in the system.

**The network boundary.** Specifications and URLs cross the network; audio payloads do not. Audio
moves through `object-storage`. This is what allows engine instances to be scaled and relocated
independently.

**The timebase boundary — now internal.** Converting between musical time and physical time is
permitted only in the transform stage of `rendering-engine`. Nothing in the render path may acquire
knowledge of the tempo model.

This boundary has not weakened in principle, but its *enforcement* has. In 0.1 it was a component
boundary: `db-transformer` was a separate program, and the render path could not have reached the
tempo model if it tried. Now the same prohibition depends on convention and review. That is a real
loss, recorded here as one, and it is the price paid for not serialising the score between two
stores.

---

## 9. Technology choices

| Choice | Applies to | Status |
|---|---|---|
| Ladybug graph database | `engine-db` | Settled |
| Cypher as the database interface | lowest HzL level, engine queries | Settled |
| One physical database, sharded by partial set | `engine-db` | Current intention |
| In-process, hosted by `rendering-engine` | `engine-db` | Current intention |
| Python | `hzl-compiler` | In use for the first version; not settled at scale |
| C++ | `rendering-engine` | Settled |
| Kafka | `message-bus` | Current intention, early |
| Multi-channel WAV objects in object storage | `rendering-engine`, `playback-server` | Current intention |
| CLAP and VST, via ARA | `playback-plugin` | Target, many open questions |

---

## 10. Open questions

Recorded, not resolved. This list is not exhaustive; it captures what is known to be open at the time
of writing.

**The database**

- The engine schema is incomplete. It holds the rendering-model concepts and has still to absorb the
  canonical ones. `engine-db` and the `canonical-db/` artefact declare seven node tables in common —
  `Score`, `Label`, `Partial`, `PartialSet`, `SoundSource`, `SoundEntity` and `SpatialPositionState`.
  Whether those become shared nodes carrying both perspectives, or stay separate, is the first
  question to answer.
- The scope `engine-db` is growing towards — the canonical model, dependency information,
  precomputation output and rendering results in one store — is stated but not designed. Nothing in
  the schema yet represents dependencies or precomputation output.
- The representation of dirty flags is not in any schema yet, and §5.2 depends on it entirely.
- `FRAME_SIZE` is referenced by `engine-db/schema/engine-schema.cypher` and declared nowhere in
  the repository. The comment there says "typically equal to the sample rate".

**Sharding**

- How partial sets are assigned to shards, whether assignment is stable, and what happens when the
  set of instances changes, are all undefined.
- What an engine instance does about a request naming partials it does not own is undefined.
- Whether a shard's `engine-db` holds only its own partials, or the whole score with only its own
  partials rendered, is undecided.

**Loading and compilation**

- The design of HzL itself is at an early stage. The number of abstraction levels, and the constructs
  at each level, are undefined.
- The implementation language for `hzl-compiler` is Python in the first version; whether that holds
  at scale is not settled.
- What `canonical-form` actually is, and how `rendering-engine` loads it, is the significant
  undecided question in the compilation half of the system. The first version's answer is a Cypher
  script of one statement, loaded by `--state` (ADR 0015); it is known not to be the answer at
  target scale.

**Rendering**

- The definition of an *integrable segment*, and how segment boundaries are chosen, is not specified
  here.
- The concurrency model within an engine instance — how many threads talk to an embedded, in-process
  Ladybug, and how that interacts with per-partial-set partitioning — is unresolved.
- The full extent of the spatial-audio timing work performed by the transform stage is not specified.

**Orchestration and delivery**

- The orchestration subsystem is at an early stage. Kafka is an intention, not a choice.
- The message schemas on `message-bus` are undefined.
- The protocol between `playback-server` and its clients is undefined.
- DAW synchronisation via ARA in `playback-plugin` carries many open questions that are not expected
  to be answered soon.

---

## 11. Why 0.2 collapsed the pipeline

Recorded because the 0.1 design was deliberate, and the reasoning that replaced it should be legible
to anyone who wonders why the pipeline went away.

Version 0.1 distributed by default: `render-orchestrator` held the graph and dispatched nearly all
computation to remote workers, keeping only phase compensation local on the grounds that it was too
simple to be worth a network hop.

The arithmetic does not support that split. A partial's specification is not much smaller than the
work of integrating or rendering it, so shipping the specification to a remote worker costs more
bandwidth than the offloaded FLOPS are worth. The same argument applied to the two databases: the
canonical and rendering models are structurally similar, differing in timebase, in units, and in
which paths through the nodes matter. Copying the whole score between two stores to express that
difference bought nothing that could not be had by holding both perspectives in one.

What survives is the part that was always load-bearing: the system is distributed, and it is
*sharded*. That was true in 0.1 and is true now. What changed is the unit of distribution — the
shard, not the pipeline stage. Each instance now does every job for the partials it owns, which is
what the random-access principle made possible in the first place.
