# RAINBOHz
## Project Summary
Cloud-scale audio synthesis.

This is a research project that aims to combine additive synthesis with cloud scalability. Additive synthesis at this larger scale is referred to as macro-scale additive synthesis.

The project is currently in an early stage, moving from an earlier proof-of-concept to a revised target architecture. Current activities are focussed on the core data model, known as the canonical form.

## Architecture

The system is distributed, and scales by sharding the partial space across
identical rendering instances rather than by chaining specialised stages
together. It has four subsystems:

**Compilation** — runs as CI/CD.

- The Hertz Language, HzL
- Hertz Language Compiler
- Hertz Language Build System
- `canonical-form` — the compiled, loadable score

**Rendering** — one component, doing all of it.

- `rendering-engine` — loads, transforms, precomputes and renders
- `engine-db` — the one graph database, hosted in process, sharded by partial set
- Object storage — WAV objects addressed by URL

**Playback** — merging the shards on demand.

- `playback-server` — sums per-shard objects and serves audio
- Playback Component
- Playback Plug-in

**Orchestration** — a message bus connecting the other three. Kafka is the
current intention, and this subsystem is at an early stage.

See [`architecture/overview.md`](architecture/overview.md) for the full
description, and §11 there for why an earlier pipelined design was collapsed
into this one.

## Programming Langauges
C++20 using clang / Coding conventions according the C++ Core Guidelines.
Python 13
TypeScript 6

## Target Platforms
This application is intended as a server-side application running on UNIX-like operating systems and/or cloud computing platforms.

The primacy goal is to target cloud platforms. This is likely to target AWS Graviton. Other platforms are also under consideration. This will demonstrate horizontal scalability, and will also leverage SIMD and similar to optimize vertical scalability.

## Project Status
This project is currently moving from its proof-of-concept phase towards a scalable architecture.

## Repository Layout

| Directory | Contents |
| --- | --- |
| `architecture/` | Target architecture: overview and diagrams |
| `engine-db/` | Schema of `engine-db`, the engine's database and the only one — under construction |
| `canonical-db/` | Canonical (composer-facing) model — design artefact, not the implementation |
| `rendering-engine/` | Rendering engine, C++ — scaffolding only |
| `playback/` | Playback component and plug-in — documentation only |
| `docs/adr/` | Architecture Decision Records |
| `scripts/` | Schema loading and shared DDL machinery |
| `tools/` | Developer tooling |
| `proof-of-concept/` | Archived earlier phase. Historical reference; not built. |

## Developer Guide
The developer guide is currently only in place for macOS, but it should be possible to follow similar steps on other operating systems.

### Build

Everything is driven from the root `Makefile`. `make help` lists the targets.

```bash
make docs                       # regenerate schema diagrams
make validate-schemas           # check the schemas load into a real Ladybug
make init-engine-db             # build a database from the engine schema
make rendering-engine        # build the rendering engine
make test-rendering-engine   # and run its tests
```

The C++ components are built with CMake behind those targets, and using CMake
directly works just as well — see [`rendering-engine/README.md`](rendering-engine/README.md).
**The first C++ build compiles Ladybug from source and takes a long time.**

`make test` runs schema validation first, because it takes seconds and needs no
compiler — a schema that does not load makes everything after it moot.

### Databases

The implementation uses one physical database, `engine-db`, hosted in process by
`rendering-engine`. It is the engine's whole database: everything in the
canonical model, plus everything the engine needs in order to determine
dependencies, precompute and render. Its schema is
`engine-db/schema/engine-schema.cypher`, and it is **under construction** — it
holds the rendering-model concepts, and the canonical-model concepts have still
to be merged in. See
[ADR 0006](docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md).

The `.cypher` schema file under `canonical-db/` is a **design artefact**. It
describes the composer-facing model and is kept loadable, but nothing loads it;
it is a source `engine-db` draws on, not something the system runs on.

Keeping both loadable is worth it: a model that a real database rejects is a
model with a mistake in it, and `make validate-schemas` catches that in seconds
— run it to see where the two stand. The databases themselves are disposable,
gitignored, and rebuild in a few seconds.

```bash
make init-engine-db             # apply the schema (safe to repeat)
make reset-engine-db            # destroy and recreate from schema
```

Each directory also carries `init_db.sh` and `reset_db.sh` for use outside make.

**Never pipe a `.cypher` file into `lbug` directly.** It reports errors on
stdout, exits 0 regardless, and silently skips statements when fed a whole
file — so schemas half-load with no diagnostic at all. Use the targets above,
which wrap [`scripts/load_schema.py`](scripts/load_schema.py). See
[ADR 0002](docs/adr/0002-apply-ddl-one-statement-per-process.md).

### Prerequisites

```bash
xcode-select --install
brew install cmake ninja ccache llvm ladybug
```

Homebrew's `llvm` is keg-only and stays off `PATH` deliberately; it supplies
`clang-tidy`, which Apple's clang does not ship. Python 3.12+ is needed for the
schema tooling.

### Ladybug

The project makes extensive use of graph databases, using [Ladybug](https://ladybugdb.com/).

There are two separate things here, and the distinction matters:

- **The Homebrew package** (`brew install ladybug`) provides the interactive
  shell, for inspecting databases by hand. Convenient, not required.
- **The embedded library** is compiled from pinned source as part of the C++
  build. This is what the components actually link against. See
  [ADR 0001](docs/adr/0001-build-ladybug-from-pinned-source.md).

Keep the two at the same version. The storage format changes between releases,
so a shell older than the linked library may not open a database the library
wrote. `brew outdated ladybug` against `RE_LADYBUG_GIT_TAG` in
[`rendering-engine/cmake/Dependencies.cmake`](rendering-engine/cmake/Dependencies.cmake)
is the check.

### Tools

#### Schema Visualisation

`tools/openCypher-tools/ladybug_schema_to_mermaid.py` generates a Mermaid UML class diagram from a LadybugDB DDL schema file.

```bash
python3 tools/openCypher-tools/ladybug_schema_to_mermaid.py engine-db/schema/engine-schema.cypher --markdown > tools/openCypher-tools/schema-diagram.md
```

Open `schema-diagram.md` in VS Code (⌘⇧V) or paste the output into [mermaid.live](https://mermaid.live) to view the rendered diagram.

#### Schema Validation

`tools/openCypher-tools/validate_ladybug_schema.py` checks a schema by executing
it against a throwaway LadybugDB, so "valid" means what the database actually
accepts rather than what a reimplemented parser believes. Failures are reported
with their source line range and the exact database error.

```bash
python3 tools/openCypher-tools/validate_ladybug_schema.py engine-db/schema/engine-schema.cypher --verbose
```

Normally reached via `make validate-schemas`. The statement splitting and error
detection are shared with the loader, in [`scripts/ladybug_ddl.py`](scripts/ladybug_ddl.py).




