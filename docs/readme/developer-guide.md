## Repository Layout

| Directory | Contents |
| --- | --- |
| `architecture/` | Target architecture: overview and diagrams |
| `hzl-compiler/` | HzL compiler, Python — first version: HzL to Cypher for `engine-db` |
| `engine-db/` | Schema of `engine-db`, the engine's database and the only one — under construction |
| `canonical-db/` | Canonical (practitioner-facing) model — design artefact, not the implementation; out of date |
| `rendering-engine/` | Rendering engine, C++ — loads, transforms, precomputes and renders; first version |
| `playback-client/` | Playback client, C++ — loops a range of a score through the default audio device; first version |
| `docs/adr/` | Architecture Decision Records |
| `scripts/` | Schema loading and shared DDL machinery |
| `tools/` | Developer tooling |
| `proof-of-concept/` | Archived earlier phase. Historical reference; not built. |

## Developer Guide
The developer guide is currently only in place for macOS, but it should be possible to follow similar steps on other operating systems.

### Build

Everything is driven from the root `Makefile`. `make help` lists the targets.

```bash
make docs                       # regenerate this README, schema diagrams and API docs
make readme                     # just this README, from docs/readme/
make validate-schemas           # check the schemas load into a real Ladybug
make init-engine-db             # build a database from the engine schema
make rendering-engine        # build the rendering engine
make test-rendering-engine   # and run its tests
make hzl-compile             # compile an HzL score to Cypher (hzl-compiler/)
make run-hzl-score           # ...and render it with the engine
make test-hzl-compiler       # the compiler's tests
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
holds the rendering-model and canonical-model concepts, but not yet the modulator
musical timelines or any dependency information. See
[ADR 0006](docs/adr/0006-retire-rendering-db-engine-db-is-the-whole-database.md).

The `.cypher` schema file under `canonical-db/` is a **design artefact**. It
describes the practitioner-facing model and is kept loadable, but nothing loads
it; it is a source `engine-db` draws on, not something the system runs on, and it
is now considerably out of date beside `engine-db`.

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




