# Changelog

Notable changes to RAINBOHz, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). There are no
releases yet, so everything is under *Unreleased*. The components version
independently; `rendering-engine`, `playback-client` and `hzl-compiler` are
each at 0.1.0.

This changelog is maintained by AI coding agents as part of each change, and
reviewed by the author. It is a summary; the git history is the full record.

## [Unreleased]

The target architecture, built from 2026-09-27 onwards.

### Added

- Engine architecture and `engine-db`, the engine's graph database schema
  (LadybugDB).
- `rendering-engine` (C++20): loads a score into `engine-db`, precomputes
  and renders it to WAV in batch mode.
- Transform stage: converts a score from musical time to physical time.
- Polytempo: several tempo maps per score, synchronised by
  `TEMPO_MAP_SYNCHRONIZATION_POINT`, resolved to physical time by the transform
  with exact anchors, uniform stretch and local compensation (ADR 0023);
  `--validate-db` checks the edges.
- `hzl-compiler` (Python): first version of the HzL compiler, from HzL to
  tables (or Cypher) for `engine-db`.
- Polytempo in HzL: `synchronization_points` on a child `tempo_map`, each tied
  to a parent in a strictly lower layer (ADR 0024); a tempo map per sound
  entity, overridable per envelope with an absolute position; every tempo map
  of the source tree compiled. HzL twins of the two polytempo examples join
  `make test-hzl-oracle`.
- Server mode for `rendering-engine`: a REST API under `/v1`, described by an
  OpenAPI 3.1.1 document, with tempo-map queries and a Postman collection.
- `playback-client` (C++): loops a range of a score through the default
  audio device, using the server's REST API.
- `playground/`: a place for musical experiments. `pg` compiles an HzL
  piece, loops it through `rendering-engine` and `playback-client`, and
  swaps in edits on `pg reload`. Example piece `beep`. No stability
  guarantee.
- Phase control: precomputed and rendered phase, `phase_free_run` with three
  meanings, and `phase: free` and `phase: null` (the default) in HzL.
- Spatial position per sound source, rendered to all eight channel modes and
  transformed from musical to physical time.
- HzL trajectories: a sound source's spatial position as a reusable file.
- Spatial-audio proofs of concept in Python and C++20, under
  `proof-of-concept/`.
- Schema tools in `scripts/`: validation against a real LadybugDB, Mermaid
  diagrams, and comment wrapping.
- `CONTRIBUTING.md`, `SECURITY.md`, `CITATION.cff` and this changelog.
- `macos-max` and `linux-max` CMake presets: fully optimised builds (Release
  with ThinLTO, Ladybug included).

### Changed

- `AGGREGATE_OF_ONE_MANY` replaced by one relationship per aggregation level.
- `proof-of-concept/` is now the home for all proof-of-concept work; its
  original sub-directories are frozen.
- The human role is called the *practitioner*, and canonical form is the
  lowest level of HzL.
- The licence, Apache-2.0, is declared in the Python package metadata, and
  every first-party source and build file starts with an SPDX licence line.
- Both C++ components are fully documented for Doxygen, and any Doxygen
  warning now fails `make docs`.
- An unterminated `/*` comment in a Cypher script is now an error naming the
  line it opened on, in `rendering-engine` and the schema tools; it used to
  hide the rest of the script without a word.
- Python dependencies are installed from a hash-checked lock,
  `requirements-dev.txt`, generated from `requirements-dev.in` by
  `make lock` (needs uv), wheels only. New targets `make lock`,
  `make lock-upgrade` and `make audit-python` (ADR 0021).
- Every C++ component shares one configuration: `.clang-format`,
  `.clang-tidy` and `.clangd` at the repository root, and the warning set,
  presets and cross toolchains in the root `cmake/`. The CMake options are
  renamed `RAINBOHZ_BUILD_TESTS`, `RAINBOHZ_ENABLE_WERROR` and
  `RAINBOHZ_SYSROOT` (were `RE_*`/`PC_*`).
- `hzlc` names the source tree's directory in the script header, not its
  absolute path, so the output is the same on every machine.
- No trailing whitespace in tracked files; `make check-whitespace`, part of
  `make test`, enforces it.
- `playback-client` plays stereo as well as mono: `stereo-from-xy`,
  `stereo-from-mid-side` and `blumlein-classic` scores play as left and
  right, on the device's first two channels, mixed down on a one-channel
  device, and in a two-channel `--output-wav`. The score sets the mode;
  `--channel-mode` makes the client fail unless the score has the one named.
- `rendering-engine` writes precomputation's and the transform's output in
  batches: each table's rows go to `engine-db` as one statement, 64 partials
  (or sources) per transaction, through a new `WriteBatch` layer (ADR 0022).
  Loading the 800-partial playground piece `convergence` went from 62 s to
  22 s on `macos-max`, with byte-identical audio.
- `hzlc` writes the score as one statement per table in chunks of 1,000
  rows (`UNWIND … CREATE` for nodes, `COPY … FROM (UNWIND …)` for
  relationships), instead of one `CREATE` clause whose binding grew faster
  than quadratically. `convergence` loads in under 1 s instead of 7 s, and a
  1,600-partial score in under 2 s instead of 72 s. Node ids now follow the
  compiler's order (they used to come out reversed), so a score with many
  partials sums them in a different order: `convergence` moves by at most
  2.4e-7.
- `hzlc` writes a score as Parquet tables and a `manifest.json` by default
  (`-o` names a directory), which `rendering-engine` loads with `COPY FROM`
  (ADR 0025). `--debug-cypher` writes the Cypher script as before, now the
  debugging form. The engine's `--state` and `PUT /v1/score` take either,
  by extension, and check a manifest before loading it. A malformed manifest
  is a 422. The 5,120-partial piece `echoes` loads in 0.11 s instead of
  9.2 s on `macos-max`, with byte-identical audio. `make test-score-forms`
  checks every example renders the same from both forms. `hzl-compiler`
  now depends on pyarrow.
- `rendering-engine` renders a frame's partial sets on several threads
  (ADR 0026), in batch and server mode. `--render-threads` sets how many;
  0, the default, uses one per hardware thread. The files and the report are
  byte-for-byte the same for every count: only the sums, files and report
  run on the calling thread, in the serial order. `make test-score-forms`
  also compares 1 thread with 8. A `macos-tsan` preset runs the tests under
  ThreadSanitizer; under it engine-db caps Ladybug's address-space
  reservation, which does not fit beside TSan's shadow memory. On `echoes`
  (32 sets and 640 partials per frame) rendering takes 0.26 s a frame on 10
  threads against 0.31 s on one, on `macos-max`: its frames are dominated
  by the serial database gather, not the kernel.

## Before this changelog

- **2024-08 to 2025-12:** C++ proof of concept: paxels, envelopes, phase
  compensation, and early YAML experiments that became HzL. Kept unchanged
  under `proof-of-concept/`.
- **2026-04 to 2026-06:** move from the proof of concept to the proposed
  architecture; first LadybugDB canonical schema.
