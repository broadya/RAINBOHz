---
status: accepted
date: 2026-08-14
decision-makers: Alan Broady
consulted: architecture/overview.md, AGENTS.md
informed: rendering-engine, future C++ components embedding Ladybug
---

# Build Ladybug from pinned source and link it statically

## Context and Problem Statement

`rendering-engine` hosts `engine-db` as an in-process Ladybug graph database
(architecture/overview.md §5.2). Ladybug therefore has to be linked into the
component's binary rather than reached over a socket.

How should the build obtain Ladybug, and should it link statically or
dynamically?

## Decision Drivers

* The component must build for several targets: Linux/arm64 (AWS Graviton) in
  production, macOS/arm64 for development, and Linux/x86_64 and Windows as
  portability checks. Cross-compilation must remain possible.
* Ladybug is an at-risk dependency. AGENTS.md rests the case for choosing it on
  the MIT-licensed C++ source being forkable and patchable if upstream stops.
  That argument only holds if we know exactly which source is compiled in.
* Deployment to a Graviton host should be as simple as possible — ideally one
  artefact.
* Build time matters for daily iteration, but less than reproducibility.

## Considered Options

* Build from pinned source via CMake `FetchContent`, link statically
* Use the system package (`brew install ladybug`, or a distro equivalent)
* Use upstream's prebuilt `liblbug` archive per platform
* Vendor a copy of the Ladybug source into this repository

## Decision Outcome

Chosen option: **build from pinned source via CMake `FetchContent`, linked
statically as the `lbug` target**, because it is the only option that survives
cross-compilation and simultaneously satisfies the survivability argument that
justified choosing Ladybug in the first place.

Specifics:

* The pin lives in `rendering-engine/cmake/Dependencies.cmake` as
  `RE_LADYBUG_GIT_TAG`, alongside the repository URL.
* The build is trimmed to the core library. Shell, language bindings,
  extensions, benchmarks, tests and the generated single-file header are all
  disabled — the last of these because it imposes a Python 3.9+ requirement at
  configure time for a header we do not use.
* `RE_LADYBUG_SOURCE_DIR` overrides the fetch with a local checkout, for
  iterating against a Ladybug working tree. It is unset by default so the
  standard build is reproducible on a clean machine.
* Ladybug's vendored third-party tree still declares `cmake_minimum_required`
  as low as 3.4, which CMake ≥ 4.0 rejects. `CMAKE_POLICY_VERSION_MINIMUM` is
  set to 3.5 for the dependency subtree.

### Consequences

* Good, because the same build configuration works for every target platform;
  the toolchain CMake is pointed at determines the output.
* Good, because the exact Ladybug source compiled into the binary is recorded
  in version control, which is what the survivability argument requires.
* Good, because a static link means Ladybug itself adds no runtime artefacts.
  This is *nearly* one self-contained binary, but not quite — see the OpenSSL
  consequence below.
* Bad, because Ladybug from v0.19.0 requires OpenSSL 3 unconditionally
  (`find_package(OpenSSL 3 REQUIRED)` for every non-WASM build, to give its
  extension installer HTTPS). There is no upstream flag to disable it, so the
  engine links OpenSSL whether or not it ever downloads an extension. By
  default this is a dynamic link, so the deployed binary needs libssl/libcrypto
  3 present at runtime, and on macOS an absolute Homebrew path is baked in.
  `RE_OPENSSL_STATIC=ON` links it statically and restores a single artefact, at
  the cost of owning OpenSSL patching for this binary. Left OFF by default so
  that OpenSSL CVEs arrive through platform package updates.
* Good, because if upstream disappears, the migration is to point
  `RE_LADYBUG_SOURCE_DIR` at an in-repository copy. Nothing else changes.
* Bad, because the first build compiles the whole of Ladybug and takes a long
  time. Mitigated with ccache, and with per-preset build trees so that switching
  configuration does not force a full rebuild.
* Bad, because the Homebrew-installed `ladybug` is now a separate thing from
  what the component links against, and the two can drift apart silently. That
  matters more than it looks: the storage format changes between releases, so a
  shell older than the linked library may not open a database the library wrote.
  The shell remains useful for inspecting databases by hand; the repository
  README says to keep the two versions matched.
* Neutral, because the pin must be bumped deliberately. The intention is to
  track the latest stable release, but by moving the pin rather than floating a
  ref — a floating ref would give up knowing exactly which source is compiled
  in, which is the premise of the survivability argument. Behaviour changes
  across a bump belong in a follow-up ADR.

### Confirmation

`make rendering-engine` configures and builds from a clean tree with no
Ladybug installed on the system. The smoke test binary reports the linked
Ladybug version, so the artefact states which source it was built from.

## Pros and Cons of the Options

### Build from pinned source via FetchContent, link statically

* Good, because it cross-compiles: source follows the toolchain.
* Good, because the version is pinned in version control.
* Good, because a single self-contained binary is produced.
* Bad, because of first-build cost.

### System package

* Good, because it is by far the fastest to set up.
* Bad, because it cannot cross-compile — there is no Graviton package on a
  macOS development machine.
* Bad, because the version is whatever the package manager has, and it lags.
  When this was written the Homebrew formula sat three minor releases behind
  upstream's latest stable.
* Bad, because it puts the survivability argument in a package maintainer's
  hands.

### Prebuilt `liblbug` archive

* Good, because it avoids the build cost while still pinning a version.
* Bad, because archives exist only per platform and per ABI, so every target
  needs its own download and its own trust decision.
* Bad, because it cannot be patched if a bug has to be fixed locally, which is
  the specific scenario the survivability argument is about.

### Vendor the source into this repository

* Good, because it is maximally reproducible and immune to upstream
  disappearing.
* Bad, because it puts a very large third-party tree into a repository whose
  own content is small, and makes upstream updates a merge exercise.
* Neutral, because this remains the fallback if upstream is abandoned; the
  chosen option is one variable away from it.

## More Information

* AGENTS.md, "Survivability" and "TerminusDB → LadybugDB Migration Rationale".
* architecture/overview.md §5.2, §9.
* `rendering-engine/cmake/Dependencies.cmake` holds the implementation.
