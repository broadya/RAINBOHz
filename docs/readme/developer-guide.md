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
| `scripts/` | Schema loading, validation, diagrams and comment wrapping, and the shared DDL machinery |
| `tools/` | Developer tooling kept outside the repository (the Ladybug Cypher VS Code extension); gitignored |
| `proof-of-concept/` | Archived earlier phase. Historical reference; not built. |
| `playground/` | Musical experiments: `pg` loops an HzL piece through the engine and reloads it on request. **No stability guarantee**; may change or vanish at any time |

## Developer Guide
The developer guide is currently only in place for macOS, but it should be possible to follow similar steps on other operating systems.

### Build

Everything is driven from the root `Makefile`. `make help` lists the targets.

```bash
make docs                       # regenerate this README, schema diagrams and API docs
make readme                     # just this README, from docs/readme/
make validate-schemas           # check the schemas load into a real Ladybug
make check-schema-comments      # show schema comments that need rewrapping
make init-engine-db             # build a database from the engine schema
make rendering-engine        # build the rendering engine
make test-rendering-engine   # and run its tests
make hzl-compile             # compile an HzL score to Cypher (hzl-compiler/)
make run-hzl-score           # ...and render it with the engine
make test-hzl-compiler       # the compiler's tests
make lint-hzl-compiler       # the compiler's lint, format and type checks
make lock                    # re-lock Python dependencies after changing them (needs uv)
make audit-python            # check the locked Python packages for known vulnerabilities
```

Python packages are installed into the root `.venv` from a hash-checked
lock, `requirements-dev.txt`, which `make lock` generates from
`requirements-dev.in` and `hzl-compiler/pyproject.toml`. Edit those, run
`make lock`, and commit the lock with them; never edit the lock by hand. See
[ADR 0021](docs/adr/0021-lock-python-dependencies-with-hashes.md).

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
schema tooling. `brew install uv` is needed only to change Python
dependencies (`make lock`).

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

`scripts/ladybug_schema_to_mermaid.py` generates a Mermaid UML class diagram from a LadybugDB DDL schema file.

```bash
python3 scripts/ladybug_schema_to_mermaid.py engine-db/schema/engine-schema.cypher --markdown > schema-diagram.md
```

Open `schema-diagram.md` in VS Code (⌘⇧V) or paste the output into [mermaid.live](https://mermaid.live) to view the rendered diagram.
`make docs` writes the repository's own diagrams to `engine-db/docs/` and `canonical-db/docs/`.

#### Schema Validation

`scripts/validate_ladybug_schema.py` checks a schema by executing
it against a throwaway LadybugDB, so "valid" means what the database actually
accepts rather than what a reimplemented parser believes. Failures are reported
with their source line range and the exact database error.

```bash
python3 scripts/validate_ladybug_schema.py engine-db/schema/engine-schema.cypher --verbose
```

Normally reached via `make validate-schemas`. The statement splitting and error
detection are shared with the loader, in [`scripts/ladybug_ddl.py`](scripts/ladybug_ddl.py).

#### Comment Wrapping

`scripts/wrap_cypher_comments.py` refills the full-line `//`
comments in Cypher DDL to 64 columns, never hyphenating: a word that crosses
the limit stays whole. Only comments change. Trailing comments, `/* ... */`
blocks, tables, `->` paths, aligned columns, list items and `NAME = value`
lines keep their shape; the docstring lists the rules.

```bash
python3 scripts/wrap_cypher_comments.py --check engine-db/schema/engine-schema.cypher
```

`make check-schema-comments` shows the diff for both schemas, and
`make format-schema-comments` rewrites them in place.




