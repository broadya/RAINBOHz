---
status: accepted
date: 2026-09-22
---

# Retire `rendering-db`; `engine-db` is the engine's whole database

## Context and Problem Statement

[ADR 0004](0004-name-the-one-database-engine-db.md) named the one physical
database `engine-db`, seeded its schema from the rendering model, and retained
`canonical-db/` and `rendering-db/` "unchanged, as design artefacts". That
retention has since stopped paying for itself.

`engine-db/schema/engine-schema.cypher` began byte-identical to
`rendering-db/schema/rendering-schema.cypher` — ADR 0004 recorded the
duplication as a known, temporary cost. The two files then diverged, in the
direction the design intended: `engine-db` was edited, and `rendering-db` was
not. By 2026-09-22 `engine-db` loaded cleanly at 48 statements while
`rendering-db` did not load at all, failing on a `CREATE REL TABLE` that named
a node table declared 160 lines below it. Nothing loaded the broken file and
nothing depended on it, so the only effect was a red `make validate-schemas`
and a standing invitation to fix a file that described nothing real.

The deeper problem is that the directory encoded an architecture the system no
longer has. A separate `rendering-db` made sense when a separate component held
it. `render-orchestrator` did, and a transformation step copied a score from
the canonical store into the rendering store. [ADR
0003](0003-collapse-the-render-pipeline-into-one-component.md) removed both the
component and the copy.

## Decision Drivers

* **Locality.** Data is sharded early, and everything belonging to a shard is
  held in one place. Two stores with a transfer between them is the shape the
  0.2 redesign removed, for bandwidth reasons recorded in
  `architecture/overview.md` §11 — and, independently, it is what the principle
  of locality argues against regardless of the arithmetic.
* **The rendering model was never a separate store.** It is a second
  perspective over a shared set of nodes: a different timebase, different
  units, and different paths through the same graph. A perspective does not
  need a database of its own, and giving it one implies a copy that does not
  happen.
* A design artefact earns its place by explaining something the real schema does
  not. Once `engine-db` held the rendering-model concepts, `rendering-db` held
  nothing that `engine-db` did not hold better, because `engine-db` is the file
  that gets edited.
* A schema that nothing loads, and that does not load, is a maintenance
  liability with no reader.
* `canonical-db/` was in a different position: it describes the
  practitioner-facing model, which `engine-db` had **not** then absorbed, so it
  still explained something the real schema did not say. `engine-db` has since
  absorbed most of it, and `canonical-db/` is now considerably out of date.

## Considered Options

* Delete `rendering-db/` and let `engine-db` carry the whole scope.
* Keep `rendering-db/` and repair it, holding it in step with `engine-db` by
  hand.
* Keep `rendering-db/` but stop validating it, as documentation-only Cypher.

## Decision Outcome

Chosen option: the first. `rendering-db/` is removed from the repository. The
schema file is preserved outside the working tree, so nothing is lost that a
later question might need, but it is no longer part of the project.

**`engine-db` is the engine's practical database, not one model's view of the
world.** Its scope is everything in the canonical model, plus everything needed
to determine dependencies, precompute and render. That is the component's
purpose restated as a data boundary: `rendering-engine` is the only component
that touches a graph database, so anything it needs has to be in the one
database it hosts.

This is a statement of intent, not of current fact. The schema holds the
rendering-model concepts and the canonical-model concepts, except the modulator
musical timelines, and nothing yet for dependencies. The intent is that it
becomes a superset of the canonical model; that is what the schema is being
grown towards, and it settles where each new concept goes: `engine-db`, because
there is nowhere else.

`canonical-db/` is retained, unchanged, as the one remaining design artefact.

This supersedes the sentence in ADR 0004 that retained both artefacts. Nothing
else in ADR 0004 changes: the name `engine-db` stands, and so does the decision
to seed its schema from the rendering model rather than the canonical one.

### Consequences

* Good, because there is now exactly one schema that anything loads, and it is
  the one that gets edited. The question "which of these files is real?" no
  longer has to be asked or answered.
* Good, because `make validate-schemas` is green again without anyone repairing
  a file that described nothing the system runs on.
* Good, because the scope of `engine-db` is now written down. ADR 0004 fixed
  where the merged schema gets written; this fixes what belongs in it, including
  the dependency and precomputation concepts that neither design artefact ever
  covered.
* Bad, because the rendering model no longer has a standalone description. What
  the rendering perspective *is*, as distinct from the canonical one, now has to
  be read out of `engine-db/schema/engine-schema.cypher` mixed with everything
  else, or from `architecture/overview.md` §5.2 in prose.
* Bad, because older ADRs and commits refer to `rendering-db/` as a live
  directory. Those references are accurate for their dates and are left alone;
  a reader following one will find nothing there.
* Neutral, because removing the third file did not answer how the node tables
  `engine-db` and `canonical-db` declare in common would merge. That has since
  been answered in `engine-db` itself, with shared nodes carrying both
  perspectives; `canonical-db/` has not followed, and is out of date.

Impact on components:

* `engine-db` — its scope is the whole canonical model plus everything needed
  to determine dependencies, precompute and render. Every new concept is added
  to `engine-schema.cypher`, because there is nowhere else.
* `rendering-engine` — the only component that touches a graph database, so
  whatever it needs has to be in the one database it hosts.
* `hzl-compiler` — emits Cypher for `engine-db` and for nothing else.
  `canonical-db/` remains a design artefact for the practitioner-facing model,
  not a load target.
* `playback-server` and orchestration (not built yet) — hold no graph
  database of their own.
* Build system — `make validate-schemas` validates two schemas,
  `engine-db`'s and the `canonical-db` artefact's.

### Confirmation

`make validate-schemas` validates two schemas and passes.

The removal is checkable: `grep -rn "rendering-db" .` over the working tree
should return hits only where the retirement is itself being recorded — this
ADR, `AGENTS.md`, and the 0.3 and 0.4 entries in `architecture/overview.md` —
plus ADRs 0002, 0003 and 0004, where the references are historical and are
deliberately not edited.

Nothing checks that `engine-db` is growing towards the scope this ADR gives it.
That is a design intent, and the only confirmation available is review.

## Pros and Cons of the Options

### Delete `rendering-db/`

* Good, because it removes a file that had diverged from the real schema and
  could only diverge further, since the real schema is the one anyone edits.
* Good, because it makes the component architecture and the directory layout
  agree: one component that touches a database, one database.
* Bad, because the rendering model loses its standalone statement, and the
  diagram generated from it.

### Keep it and repair it

* Good, because the rendering perspective keeps a description that can be read
  and diagrammed on its own.
* Bad, because keeping it truthful means hand-mirroring every `engine-db` edit
  into a file nothing loads. That work has no forcing function, so it would be
  skipped, and the file would go stale silently — which is exactly what had
  already happened.

### Keep it, stop validating it

* Good, because it costs nothing in build time and the prose survives.
* Bad, because an unvalidated schema that already does not load is worse than no
  schema: it looks authoritative and is wrong, and the one cheap check that
  would have caught that has been turned off.

## More Information

Supersedes the retention of `rendering-db/` decided in
[ADR 0004](0004-name-the-one-database-engine-db.md). The duplication this ADR
resolves is the one ADR 0004 recorded as a known temporary cost.

The component reasoning is [ADR
0003](0003-collapse-the-render-pipeline-into-one-component.md): two databases
existed because two components existed, and collapsing the components is what
eventually made the second database indefensible. `architecture/overview.md` 0.4
records the resulting shape; §11 holds the bandwidth argument.

The part worth re-examining is the scope claim. "Everything needed to determine
dependencies, precompute and render" is easy to write and hard to bound — if it
turns out that precomputation output is better held outside the graph, as files
addressed from it, then `engine-db` holds references rather than results and
this ADR overstates its reach.

Implementation: `engine-db/`, and the `EngineDb` wrapper in
`rendering-engine/include/rainbohz/rendering_engine/engine_db.hpp`.
