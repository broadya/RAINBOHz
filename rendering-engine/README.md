# rendering-engine

**Status: partly implemented.** The engine stands up `engine-db`, loads it,
reads and checks a render request, precomputes every partial's integrable
segments and frame entries (first version: linear frequency envelopes only;
see [docs/precomputation.md](docs/precomputation.md)), and renders the
requested frames to WAV files (first version: mono, linear curves, labels
ignored; see [docs/rendering.md](docs/rendering.md)). It does not yet
transform anything.

This directory holds the component identified as `rendering-engine` in
[architecture/overview.md](../architecture/overview.md). What exists today is
the batch test harness end to end: an in-memory Ladybug database is created,
the `engine-db` schema is applied to it, and an initial database state is
loaded from a Cypher file named on the command line, with every failure
attributed to its source line; a YAML render request, naming the frames to
render and the labels that select their partials, is read and checked against
what loaded; every partial is precomputed; and each requested frame is
rendered to a file.

## What the component will do

From `architecture/overview.md` §5.2:

- It is a **C++** program (this is Settled in §9, not a candidate choice).
- It **hosts `engine-db`**, a Ladybug graph database, **in process**. This is
  the one physical database in the system, sharded by partial set.
- It **performs every operation that needs the graph**: loading `canonical-form`,
  transforming the canonical model into the rendering model in place,
  precomputing phase integration, phase compensation, and rendering frames to
  multi-channel WAV objects in `object-storage`.
- It **determines the calculation delta** from dirty flags in the graph, and
  deliberately never inspects `object-storage` to do so.

The framing that shapes the eventual code: computation lives where its data
lives. This component is the data's home, so the computation belongs here.

Version 0.1 of the architecture split this work across `db-transformer`,
`precompute-worker` and `render-worker`, with this component dispatching jobs
to them over the network. That split was abandoned because the bandwidth cost of
shipping specifications exceeded the compute it offloaded. See overview.md §11.

See [docs/design.md](docs/design.md) for the component design and its open
questions, [docs/precomputation.md](docs/precomputation.md) for the
precomputation step, and [docs/rendering.md](docs/rendering.md) for the
rendering step that follows it — of which the largest is that the engine schema is still under
construction, holding the rendering-model concepts but not yet the canonical
ones.

## Run modes

Three modes are anticipated by the architecture. **Only `test` is
implemented**; the other two are recognised on the command line and rejected,
so that the mode axis exists in the code rather than being retrofitted later.

| Mode | Lifetime | Input | Output | Status |
| --- | --- | --- | --- | --- |
| `test` | batch, then exits | a Cypher state file and a YAML render request | WAV files in a local directory | implemented for the first versions of precomputation and rendering |
| `distributed` | long-running server | requests over a mechanism not yet chosen | object storage | not implemented |
| `standalone` | long-running server | as `distributed` | a local directory | not implemented |

`test` is the harness [docs/design.md](docs/design.md) §3 describes, and it is
not a lesser path: until the orchestration subsystem is more than a sketch, it
is the only one that can actually be run.

## Usage

```
rendering-engine --mode test --state <file.cypher> --request <file.yaml> [options]

  -m, --mode  test|distributed|standalone   Run mode [test]
  -s, --state <file.cypher>                 Initial database state (required in test mode)
  -r, --request <file.yaml>                 Frames and labels to render (required in test mode)
      --schema <file.cypher>                Schema DDL to apply
                                            [../engine-db/schema/engine-schema.cypher]
  -o, --out-dir <dir>                       Rendered WAV files, one per frame
                                            [current directory], created if absent
      --per-set-wav                         Also write each partial set's contribution
                                            to a frame as its own file
      --db-path <path>                      Put engine-db on disk instead of in memory
      --validate-db                         Check engine-db's integrity after loading;
                                            stop before precomputation if it fails
  -v, --verbose                             Report every statement, not just failures
```

What `test` mode does today:

1. Reads `--request` and checks its shape. A malformed request stops the run
   here, before anything is loaded.
2. Opens an empty `engine-db` — in memory, which is the deployed configuration.
3. Applies `--schema`, one Cypher file of DDL.
4. Applies `--state`, one Cypher file of initial database state.
5. Prints what the schema declared and what the state actually loaded.
6. With `--validate-db`, checks the integrity of what loaded: no cycles or
   merged lists in any `NEXT_POINT` list, and the columns the engine reads in
   their valid ranges. A violation stops the run here, with exit 1. See
   [docs/integrity.md](docs/integrity.md).
7. Precomputes every partial ([docs/precomputation.md](docs/precomputation.md)).
8. Lists the requested frames and checks every requested label against the
   `Label` nodes that loaded.
9. Renders each requested frame to `--out-dir/frame-NNNNNN.wav`, a 32-bit
   float mono WAV of one frame ([docs/rendering.md](docs/rendering.md)); with
   `--per-set-wav`, also `frame-NNNNNN.set-NNNNNN.wav` per contributing
   partial set. A frame with nothing in scope is a file of silence. Then
   exits 0, or 1 if any statement did not apply, an integrity check failed,
   a partial could not be precomputed, any label is unknown, or any frame
   could not be rendered. Labels are checked but do not yet select partials.

### The render request

```yaml
frames:
  - frame: 0                  # an integer, 0 or greater; 0 is the first frame
    labels: [strings, brass]  # only partials carrying these labels
  - frame: 2                  # no labels: every partial
  - frame: 3
    labels: []                # also every partial
```

Each frame may appear once, because each becomes one WAV file. Every label
must exist in `engine-db`; a misspelt one is an error rather than a selection
that renders silence. Unknown keys, repeated keys and anything that is not a
plain decimal frame number are refused, and every problem is reported with its
line, all at once. The format is specified in
[docs/design.md](docs/design.md) §3.2.

Two files, named separately. The engine does not read a directory: which files
a run applies, and in what order, is something the command line says outright.
`--schema` has an in-repo default so that a developer build picks up a schema
edit without a reconfigure; `--state` has none, because a run must not silently
load a score nobody asked for.

The row census in step 5 is the part that says the score arrived. Statement
counts only say the file parsed and was accepted — a `CREATE` whose `MATCH`
found nothing is accepted too, and stores nothing.

Every statement is submitted individually so that a failure carries its file
and source lines, and **the run continues past a failure** so that one bad
statement does not hide the rest. See
[ADR 0005](../docs/adr/0005-load-cypher-into-engine-db-in-process.md).

`--db-path` is a debugging affordance, not a deployment option: it leaves a
database behind that can be opened in the `lbug` shell.

```bash
make run-rendering-engine                                        # the examples, below
make run-rendering-engine RE_STATE=my.cypher RE_REQUEST=my.yaml  # against your own
make run-rendering-engine RE_ARGS=--validate-db                  # with the integrity checks
make run-rendering-engine RE_ARGS="--verbose --per-set-wav"      # one file per partial set too
```

The default schema and score live in [`engine-db/`](../engine-db/), beside
each other: [`schema/engine-schema.cypher`](../engine-db/schema/engine-schema.cypher)
and [`examples/minimum-score.cypher`](../engine-db/examples/minimum-score.cypher).
The default request is [`examples/minimum-request.yaml`](examples/minimum-request.yaml);
the example score holds no labels, so it asks for every partial. `make` sets
`--out-dir` to `build/<preset>/frames` so that output stays out of the
repository. A second, richer score, `two-partial-sets.cypher`, with its
request `examples/two-partial-sets-request.yaml`, exercises two partial
sets, glides, fades, a mid-frame start and partials leaving the audio
range; each score has a JSON mirror beside it for the oracle
(`test/oracle/render_oracle.py`, [docs/rendering.md](docs/rendering.md)
§11.1), which checks the engine's output against an independent NumPy
implementation.

The example score is example data, not a specification of `canonical-form` —
how a score actually reaches `engine-db` is still open (overview.md §10).

## Layout

```
rendering-engine/
├── CMakeLists.txt          component build
├── CMakePresets.json       per-platform configurations
├── .clang-format           formatting (close to Ladybug's own)
├── .clang-tidy             C++ Core Guidelines analysis
├── cmake/
│   ├── Dependencies.cmake  Ladybug, CLI11 and yaml-cpp acquisition, pinned
│   ├── ProjectWarnings.cmake
│   └── toolchains/         cross-compile configurations
├── examples/
│   ├── minimum-request.yaml  the render request `make run-rendering-engine` passes
│   └── two-partial-sets-request.yaml  for engine-db/examples/two-partial-sets.cypher
├── include/rainbohz/rendering_engine/
│   ├── amplitude_maths.hpp  amplitude ramp, dB conversion, roll-off
│   ├── cypher_script.hpp   statement splitting, with line attribution
│   ├── engine_db.hpp       in-process engine-db wrapper
│   ├── frame_plan.hpp      render segments, chunk cuts, per-set driver
│   ├── integrity.hpp       engine-db integrity checks (--validate-db)
│   ├── options.hpp         command line and run modes
│   ├── precomputation.hpp  segments, start phases, frame scope and entries
│   ├── render_kernel.hpp   the hot loop over Structure-of-Arrays state
│   ├── render_request.hpp  the test-mode YAML render request
│   ├── rendering.hpp       per-frame gather, validation, loop and report
│   ├── sample_value.hpp    the audio sample type
│   ├── script_loader.hpp   applying Cypher files to engine-db
│   ├── segment_maths.hpp   a segment's frequency and phase
│   ├── units.hpp           unit types for physical quantities
│   └── wav_writer.hpp      32-bit float WAV output (dr_wav)
├── src/
│   ├── main.cpp            entry point and test-mode driver
│   ├── cypher_script.cpp
│   ├── engine_db.cpp
│   ├── frame_plan.cpp
│   ├── integrity.cpp
│   ├── options.cpp
│   ├── precomputation.cpp
│   ├── render_kernel.cpp
│   ├── render_request.cpp
│   ├── rendering.cpp
│   ├── script_loader.cpp
│   └── wav_writer.cpp
└── test/
    ├── oracle/
    │   ├── render_oracle.py      independent NumPy check of one frame
    │   └── run_oracle_check.py   CTest driver: engine, then oracle per frame
    ├── test_support.hpp          a hand-written WAV reader for the tests
    ├── test_amplitude_maths.cpp
    ├── test_cypher_script.cpp
    ├── test_engine_db.cpp
    ├── test_frame_plan.cpp
    ├── test_integrity.cpp
    ├── test_precomputation.cpp
    ├── test_render_kernel.cpp
    ├── test_render_request.cpp
    ├── test_rendering.cpp
    ├── test_script_loader.cpp
    ├── test_segment_maths.cpp
    ├── test_units.cpp
    └── test_wav_writer.cpp
```

## Building

From the repository root:

```bash
make rendering-engine        # configure + build
make test-rendering-engine   # + run tests
make run-rendering-engine    # + run the smoke test binary
```

Or with CMake directly, from this directory:

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

**The first build compiles Ladybug from source and takes a long time.**
Subsequent builds are incremental and ccache-accelerated.

The build fetches Ladybug from the pinned repository and tag. It does not use any
checkout you happen to have locally — that would make the build depend on
machine state. If you specifically want to build against a local working tree
(patching Ladybug, bisecting a bug), say so explicitly:

```bash
cmake --preset macos-debug -DRE_LADYBUG_SOURCE_DIR=/path/to/ladybug
```

### Presets

| Preset | Purpose |
| --- | --- |
| `macos-debug` | Local development. Default on macOS. |
| `macos-release` | RelWithDebInfo. |
| `macos-asan` | Debug + AddressSanitizer + UBSan. |
| `linux-debug` / `linux-release` | Native Linux; serves both arm64 and x86_64. |
| `windows-debug` / `windows-release` | MSVC, from a Developer Command Prompt. |
| `linux-aarch64-cross` | Cross-compile to Graviton. Needs `-DRE_SYSROOT=...`. |
| `linux-x86_64-cross` | Cross-compile to Linux x86_64. Needs `-DRE_SYSROOT=...`. |

## Portability

Production is Linux/arm64 (AWS Graviton); development is macOS/arm64. Linux
x86_64 and Windows are supported as portability checks, not deployment targets
— keeping them configurable stops platform assumptions accumulating silently.

Everything is built from source, including Ladybug, precisely so that the build
follows whatever toolchain CMake is pointed at. No prebuilt binary dependency is
used, because prebuilt archives exist only per-platform.

Two caveats worth stating plainly:

- **The cross-compile toolchain files are untested.** They record the intended
  configuration and require a sysroot the developer supplies. Ladybug's vendored
  third-party tree is large; first real use should be expected to need
  adjustment.
- **Only macOS/arm64 has actually been built.** The other configurations are
  asserted by the build files, not demonstrated. There is no CI.
- **`macos-asan` currently fails to link.** Ladybug's vendored zstd calls
  `__asan_unpoison_memory_region` directly; the linker reports it undefined
  even though it locates the symbol in `libclang_rt.asan_osx_dynamic.dylib`.
  Reproduced with Xcode Command Line Tools 16.4 (Apple clang 17.0.0, no full
  Xcode installed) — an ASan/ld64 or compiler-rt versioning issue, not
  something in this component's own build files. Unresolved; a full Xcode
  install is the next thing to try.

## Dependencies

| Dependency | Version | Acquisition |
| --- | --- | --- |
| [Ladybug](https://github.com/LadybugDB/ladybug), built from [our fork](https://github.com/broadya/ladybug) | pinned, see `cmake/Dependencies.cmake` | CMake FetchContent |
| [CLI11](https://github.com/CLIUtils/CLI11) | pinned, see `cmake/Dependencies.cmake` | CMake FetchContent |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | pinned, see `cmake/Dependencies.cmake` | CMake FetchContent |
| [dr_wav](https://github.com/mackron/dr_libs) | pinned by commit, see `cmake/Dependencies.cmake` | CMake FetchContent |
| GoogleTest | pinned, see `test/CMakeLists.txt` | CMake FetchContent |
| Python 3 with NumPy | whatever is first on `PATH` | for the oracle tests only; skipped if absent |

CLI11 is header-only and is used by `src/options.cpp` alone; yaml-cpp is built
static and used by `src/render_request.cpp` alone; dr_wav is one header,
carried by `src/wav_writer.cpp` alone ([ADR 0009](../docs/adr/0009-write-wav-files-with-dr-wav.md)).
All three are linked PRIVATE and appear in no public header, so callers of
this component do not acquire them — the same rule Ladybug is held to.

Ladybug is built trimmed to the core library: no shell, no language bindings,
no extensions, no single-file header.

### OpenSSL

Ladybug requires **OpenSSL 3** unconditionally from v0.19.0 onward — its
CMakeLists does `find_package(OpenSSL 3 REQUIRED)` for every non-WASM build, to
give the extension installer HTTPS. There is no upstream flag to turn it off,
so this component links OpenSSL whether or not it ever downloads an extension.

By default it links **dynamically**, so:

- The deployed binary needs libssl/libcrypto 3 present at runtime. Any current
  Linux base image has this; a Graviton sysroot needs it too.
- On macOS the Homebrew path (`/opt/homebrew/opt/openssl@3/...`) is baked in as
  an absolute path, so a locally built binary is not portable to another Mac.

`-DRE_OPENSSL_STATIC=ON` links it statically and gives back a single
self-contained artefact. The trade-off is that you then own OpenSSL patching
for this binary rather than getting it from platform package updates. Dynamic
is the default for that reason.

### Tracking Ladybug releases

The intention is to build against the **latest stable** Ladybug, but by moving
the pin deliberately rather than floating a ref — Ladybug is a community fork of
an archived project, and per AGENTS.md the survivability argument depends on
knowing exactly which source is compiled in.

To check for a newer release and bump:

```bash
git ls-remote --tags --refs https://github.com/LadybugDB/ladybug \
  | awk -F/ '{print $NF}' | sort -V | tail
```

For now the build uses our fork, [broadya/ladybug](https://github.com/broadya/ladybug):
v0.20.4 plus upstream's own SERIAL primary-key index fix
([LadybugDB/ladybug#1028](https://github.com/LadybugDB/ladybug/pull/1028)),
backported because it is merged but not yet released
([ADR 0011](../docs/adr/0011-carry-upstreams-serial-fix-until-released.md)).

**When a release includes #1028**, leave the fork: set `RE_LADYBUG_GIT_REPOSITORY`
back to `https://github.com/LadybugDB/ladybug` and `RE_LADYBUG_GIT_TAG` to that
release. Then check that a SERIAL lookup still uses the index; `PROFILE MATCH
(n:T) WHERE n.id = 7 RETURN n.v` on a SERIAL-keyed table must read one row.

**If a release comes out without #1028**, carry the fix forward instead. In a
clone of the fork, with upstream as a remote, branch from the new release and
cherry-pick the fix commits listed in the tag message of `v0.20.4-rainbohz.2`:

```bash
git fetch upstream --tags
git checkout -b rainbohz/vX.Y.Z vX.Y.Z
git cherry-pick -x <commits from the v0.20.4-rainbohz.2 tag message>
# build, run test/optimizer/optimizer_test and the e2e suite, then:
git tag -a vX.Y.Z-rainbohz.1 -m "Ladybug vX.Y.Z plus RAINBOHz patches"
git push origin rainbohz/vX.Y.Z vX.Y.Z-rainbohz.1
```

Then edit `RE_LADYBUG_GIT_TAG` in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake),
reconfigure from a clean build tree, and run the tests. A bump crosses an
upstream API boundary often enough that a clean rebuild is the only honest
check. A clean tree matters for a second reason: the repository and tag are
cache variables, so an existing build tree keeps the old ones. Record behaviour
changes in `docs/adr/`.

## Conventions

- C++20, per the repository README. Ladybug is C++20; going higher buys nothing
  until a C++23 feature is actually needed and would narrow the usable compiler
  set on Linux and Windows.
- C++ Core Guidelines, enforced as far as `cppcoreguidelines-*` in `.clang-tidy`
  reaches. It is guidance, not a gate — `WarningsAsErrors` is empty until there
  is code worth defending.
- Types `PascalCase`, functions and variables `camelCase`, private members
  trailing `_`, constants `kPascalCase`.
- Sources are listed explicitly in CMake, never globbed.
- Ladybug's headers do not appear in this component's public headers. Callers
  should not acquire a transitive dependency on `lbug::main` types.
