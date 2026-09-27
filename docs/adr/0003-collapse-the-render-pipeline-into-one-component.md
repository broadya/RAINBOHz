---
status: accepted
date: 2026-09-18
decision-makers: Alan Broady
consulted: architecture/overview.md, AGENTS.md
informed: rendering-engine, hzl-compiler, playback-server
---

# Collapse the render pipeline into one component, over one database

## Context and Problem Statement

`architecture/overview.md` 0.1 described rendering as a pipeline of four
components. `render-orchestrator` held `rendering-db` but performed almost no
computation: it dispatched integrable-segment jobs to `precompute-worker` and
frame jobs to `render-worker` over the network, keeping only phase compensation
local. Upstream, `db-transformer` was a separate program that read `canonical-db`
and wrote `rendering-db`.

That design followed from a principle stated in §3: *distributed by default,
with deliberate exceptions*. Is the default right?

## Decision Drivers

* Bandwidth versus FLOPS. A partial's specification is not much smaller than the
  work of integrating or rendering it.
* The two models are structurally similar. They differ in timebase, in units, and
  in which paths through the nodes matter — not in what entities exist.
* Locality of reference. The renderer's input is the graph.
* Sharding was always the scaling mechanism, in both designs.
* The random-access principle: any frame is renderable in isolation from
  precomputed phase, so an instance holding a partial can render any frame for
  it without consulting anything else.

## Considered Options

* Keep the 0.1 pipeline: four components, two databases, work shipped to workers.
* Collapse the workers into the orchestrator, keep two databases.
* Collapse the workers **and** `db-transformer` into one component, over one
  physical database.

## Decision Outcome

Chosen option: the third. `render-orchestrator` becomes `rendering-engine` and
absorbs `db-transformer`, `precompute-worker` and `render-worker`. The two
databases become one, `engine-db`, hosted in process and sharded by partial
set.

The arithmetic does not support the 0.1 split. Shipping a specification to a
remote worker costs more bandwidth than the offloaded compute is worth, so the
network hop is a loss on every job rather than a scaling win. The same argument
applies to the two stores: copying the whole score between them to express a
difference of timebase and units buys nothing that cannot be had by holding both
perspectives over one set of nodes.

What survives is the part that was always load-bearing. The system is still
distributed and still sharded. What changed is the unit of distribution — the
shard, not the pipeline stage.

The §3 principle inverts accordingly: *locality of reference* and *sharding, not
pipelining* replace *distributed by default*.

### Consequences

* Good, because the transform reads and writes the same store and never
  serialises the score in order to convert it.
* Good, because an engine instance already holds the specifications it needs, so
  no job carries one.
* Good, because local and remote worker functions no longer need to look alike at
  the call site — a design constraint 0.1 imposed that bought nothing once the
  remote case disappeared.
* **Bad, because the timebase boundary stops being enforced structurally.** In
  0.1, `db-transformer` was a separate program and the render path could not have
  reached the tempo model if it tried. The same prohibition now depends on
  convention and review. This is the real cost of the decision and should not be
  glossed: a tempo-aware type reaching the render path is now possible, and must
  be treated as a defect of the same severity as a correctness bug.
* Bad, because a single component now has a very wide responsibility — loading,
  transformation, precomputation and rendering — and will need internal structure
  to stay comprehensible.
* Neutral, because the two existing schema files become design artefacts rather
  than implementations. They remain useful, remain valid Cypher, and remain worth
  validating; they are simply not what the system loads. The engine schema
  will be formed by merging concepts from both, and does not exist yet.
* Neutral, because `audio-service` and `summing-worker` merge into
  `playback-server` as a consequence of the same reasoning, though summing may
  still fan out.

### Confirmation

`architecture/overview.md` 0.2 §5, §7, §8 and §11 record the resulting shape.
The component is `rendering-engine/`, and `rendering-engine/docs/design.md`
carries the component-level detail.

The decision is not confirmed by anything executable yet: at the time of writing
the component is still scaffolding, and the largest open question — the unified
engine schema — is exactly the thing this decision creates the need for.

## More Information

The bandwidth argument is the one to re-examine if this is ever revisited. It
depends on the ratio between a partial's specification size and the compute
required to render it, and on network cost per byte. Both move over time. If a
future partial representation becomes very much smaller relative to its render
cost, the 0.1 split becomes defensible again.

Superseded by this ADR, in `architecture/overview.md` 0.1: `db-transformer`,
`precompute-worker`, `render-worker`, `render-orchestrator`, `canonical-db` and
`rendering-db` as components, `audio-service`, and `summing-worker`.
