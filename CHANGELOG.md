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
- `hzl-compiler` (Python): first version of the HzL compiler, from HzL to
  Cypher for `engine-db`.
- Server mode for `rendering-engine`: a REST API under `/v1`, described by an
  OpenAPI 3.1.1 document, with tempo-map queries and a Postman collection.
- `playback-client` (C++): loops a range of a score through the default
  audio device, using the server's REST API.
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

## Before this changelog

- **2024-08 to 2025-12:** C++ proof of concept: paxels, envelopes, phase
  compensation, and early YAML experiments that became HzL. Kept unchanged
  under `proof-of-concept/`.
- **2026-04 to 2026-06:** move from the proof of concept to the proposed
  architecture; first LadybugDB canonical schema.
