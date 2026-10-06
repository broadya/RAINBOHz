---
status: accepted
date: 2026-09-24
---

# Write WAV files with a library built for it, dr_wav, not a hand-written writer

## Context and Problem Statement

`rendering-engine` writes each rendered frame as a 32-bit float WAV file
(`rendering-engine/docs/rendering.md` §5). The first draft of the design
specified a hand-written RIFF/WAVE writer, as the proof of concept had. The
author decided (2026-09-24) that WAV output should come from a library
designed for the purpose. Which one?

## Decision Drivers

* **Correctness of the container.** Float WAV needs the 18-byte `fmt `
  chunk and a `fact` chunk; hand-written writers routinely get one of these
  wrong and the file plays anyway in lenient readers, so the mistake is
  found late.
* **The dependency rules of AGENTS.md.** Built from pinned source via
  FetchContent, never taken from the system; included as a SYSTEM header;
  never exposed in a public header of this component.
* **Weight.** The engine links Ladybug, which is already large; the WAV
  library should not add a build system of its own or optional codecs.
* **Reach.** Multi-channel and 64-bit float output are anticipated
  (`rendering.md` §13); a change of container or format later should be a
  parameter, not a rewrite.
* **Licence.** The repository is public FOSS.

## Considered Options

1. **Hand-written writer** (the previous design).
2. **dr_wav** (`mackron/dr_libs`): one C header, public domain or MIT-0 at
   the user's choice, reads and writes PCM and IEEE float WAV, any channel
   count, RIFF, W64 and RF64 containers.
3. **libsndfile**: the canonical C library, LGPL-2.1, CMake build, many
   formats, optional Ogg/Vorbis/FLAC/Opus dependencies.
4. **AudioFile** (`adamstark/AudioFile`): C++ header-only, MIT, WAV and
   AIFF, holds a whole file in nested vectors.

## Decision Outcome

Chosen option: **2, dr_wav**, because it meets the correctness driver with
the least weight: a single header, no build system, no transitive
dependencies, a permissive licence, and float, multi-channel and 64-bit
output already supported. It fits the FetchContent rule directly, and being
C it is trivially kept out of public headers.

libsndfile is the more capable library, and would be the choice if the
engine ever needs formats beyond WAV; its build and its LGPL obligations are
not worth carrying for one container. AudioFile's whole-file-in-memory
model duplicates the frame buffer for no benefit. The hand-written writer
was the option being replaced.

### Consequences

* Good, because the container is produced by code exercised by many users,
  and the engine's own writer shrinks to a thin wrapper.
* Good, because channels, bit depth and container are parameters of one
  call, so the extensions in `rendering.md` §13 need no writer changes.
* Bad, because dr_libs does not tag releases per header; the pin is a
  commit hash in `cmake/Dependencies.cmake`, and bumping it means reading
  the header's changelog rather than a release note.
* Neutral, because dr_wav is one author's project. It is small enough that
  the survivability argument of ADR 0001 applies with even more force: the
  header can be vendored or patched in an afternoon.

Impact on components:

* `rendering-engine` — writes every frame through `src/wav_writer.cpp`, the
  only translation unit that includes `dr_wav.h`, linked PRIVATE.
* `playback-client` — reads the rendered frames with dr_wav too, from the same
  pinned commit. The two components bump the pin together.
* Object storage — holds 32-bit float WAV objects. A different format there
  reopens this decision.
* `playback-server` (not built yet) — the per-shard objects it sums are float
  WAV, so dr_wav is the natural reader for it as well.

### Confirmation

`src/wav_writer.cpp` is the only translation unit that includes `dr_wav.h`,
and it is linked PRIVATE, as CLI11 and yaml-cpp are; a public header naming
a dr_wav type is a review failure. `test_wav_writer` reads a written file
back with the same library and also checks the raw chunk layout by hand, so
that a library regression in the container is caught, not trusted. The
oracle (`rendering.md` §11.1) parses the RIFF chunks itself in NumPy, which
is a third, independent check of the container on every oracle run.

## More Information

* `rendering-engine/docs/rendering.md` §5 and §11.
* ADR 0001 for the dependency policy this follows.
* Revisit if a format other than WAV is required for `object-storage`, at
  which point libsndfile is the candidate.
