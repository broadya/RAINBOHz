---
status: accepted
date: 2026-10-11
---

# Install Python dependencies from a hash-checked lock, made with uv

## Context and Problem Statement

The C++ dependencies are built from pinned source (ADR 0001): a release tag
or a commit SHA for each. The Python ones were not pinned at all.
`requirements-dev.txt` and `hzl-compiler/pyproject.toml` gave version
ranges (`sympy>=1.12`), and `make venv` ran a plain `pip install`, plus an
unpinned upgrade of pip itself. Whatever release on PyPI matched a range
when the venv was built went into the root `.venv`, which runs the compiler,
the schema tools, Ladybug's code generation and every Python-driven test.
A hijacked or broken release would have arrived unannounced. Building an
sdist would also have fetched its build backend with no check.

How should the root venv's Python packages be pinned and verified?

## Decision Drivers

* **Integrity.** What is installed is exactly what was reviewed: same
  version, same file bytes.
* **One lock for every platform.** Development is macOS arm64. The
  deployment image and Graviton are Linux aarch64.
* **Normal builds need nothing new.** Building and testing should need only
  the Python the venv is made from.
* **Fail, never skip** (AGENTS.md). A lock that has fallen behind its inputs
  must stop the build, not quietly produce a venv that lacks something.
* **Not upload-time-of-attack.** Most malicious PyPI releases are found and
  removed within days. A lock taken at the wrong moment should not catch one.

## Considered Options

1. **uv** (`uv pip compile`), installed from Homebrew, used only to make the
   lock.
2. **pip-tools** (`pip-compile`), installed into the venv.
3. **pip's own `pip lock`** (`pylock.toml`).
4. **Exact `==` pins without hashes.**

## Decision Outcome

**uv**, as a locking tool only. The arrangement:

* `requirements-dev.in` is hand-edited and holds ranges. Together with
  `hzl-compiler/pyproject.toml` and its `dev` extra, `make lock` resolves it
  into `requirements-dev.txt`: every package, transitive dependencies
  included, at one version, with the SHA-256 of every file published for
  that version. The lock is universal (one file for macOS and Linux) and is
  never edited by hand.
* `make lock` keeps existing pins and changes only what the inputs require.
  `make lock-upgrade` moves everything to the newest allowed version. Both
  ignore releases younger than `LOCK_COOLDOWN` (7 days) via
  `--exclude-newer`.
* `make venv` uses plain pip. It runs `pip install --require-hashes
  --only-binary :all:` from the lock, including pip and setuptools. It then
  installs the compiler editable with `--no-deps --no-build-isolation`, so
  nothing outside the lock is downloaded for it. Finally `pip check` fails
  the build when the lock lacks a dependency the compiler declares.
* `make audit-python` runs pip-audit over the lock against the published
  vulnerability databases. It is not part of `make test`: it needs the
  network, and its answer changes as advisories are published, so a green
  test run would stop meaning that the code is right.

uv meets every driver. Its universal resolution gives one lock for both
platforms, which pip-tools cannot: it locks for the machine it runs on.
`--exclude-newer` gives the cooldown, and pip-tools has none. uv comes from
Homebrew, as cmake and ninja do, so the tool that makes the lock does not
itself arrive by the unverified route it replaces. `pip lock` writes
`pylock.toml`, which pip cannot yet install from, and it locks for one
platform. `==` pins without hashes stop a range from drifting, but they do
not stop a file being replaced or a release being hijacked, so they protect
against the wrong thing.

### Consequences

* Good, because every third-party Python file in the venv is checked against
  a hash in a reviewed, committed file. A tampered or substituted file
  stops the install.
* Good, because only wheels are installed, so no package's build code runs
  on install.
* Good, because a dependency change shows up as a diff of the lock, which is
  where it is reviewed.
* Bad, because changing a dependency is now two steps (edit, `make lock`)
  and needs uv, one more tool from one company. If uv ever goes away,
  pip-tools reads the same inputs and writes the same format:
  `pip-compile --generate-hashes`, run once per platform.
* Bad, because security fixes arrive only when someone runs
  `make lock-upgrade`. `make audit-python` is how to find out that one is
  needed.
* Neutral, because binary-only means each locked version must publish a
  wheel for every target. A package without one fails the install by name.
  NumPy 2.5's Linux aarch64 wheel needs glibc 2.27 or later, which Debian
  bookworm (the planned base image) has and Amazon Linux 2 does not.

Impact on components:

* `hzl-compiler`: its runtime and development dependencies stay as ranges in
  its `pyproject.toml`. What is installed is the lock's pins. A new
  dependency needs `make lock`, or `make venv` fails.
* `rendering-engine`, `playback-client`: their Python-driven tests and
  Ladybug's code generation run in the same venv, so they get locked
  packages unchanged. Ladybug's build scripts use only the standard library.
* The deployment image (not built yet): it should install from the same
  lock with the same pip flags.

### Confirmation

`make venv` enforces hashes, wheels only and `pip check` on every build.
Nothing checks the cooldown or upgrade cadence beyond review of the lock's
diff.

## More Information

`requirements-dev.in`, `requirements-dev.txt` and the Python section of the
root `Makefile` implement this. ADR 0001 is the C++ counterpart.
`proof-of-concept/` keeps its own venvs and is not covered.
