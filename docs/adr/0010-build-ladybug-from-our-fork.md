---
status: superseded by ADR-0011
date: 2026-09-25
---

# Build Ladybug from our fork, carrying the SERIAL primary-key index fix

## Context and Problem Statement

Ladybug v0.20.4 does not use the primary-key hash index when a SERIAL key is
compared with an INT64 value. The binder casts the value to SERIAL and names the
bound cast `CAST_TO_SERIAL`. The optimizer counts a value as constant only if
it is a literal, a parameter, or a function named exactly `CAST`, so the lookup
falls back to a full scan plus filter.

This is the path every `int64_t` id bound from C++ takes. On a 1M-row table the
lookup cost 0.46 ms against 0.12 ms for the index. It grows with the table, to
1.7 ms at 4M rows, while the index lookup stays flat. `engine-db` uses SERIAL
keys throughout, so every per-statement lookup paid that cost.

The bug is present on upstream `main`. The fix has not been proposed upstream
yet; that conversation is to be had with the Ladybug community first.

## Decision Drivers

* Lookups must not scan whole tables. Per-frame and per-statement cost must not
  grow with the size of the piece.
* ADR 0001 requires knowing exactly which Ladybug source is compiled in.
* Changes to Ladybug should be as small as possible, so they are easy to carry
  across upstream releases and easy to offer upstream.

## Considered Options

* Patch Ladybug, in our fork, pinned by tag
* Work around the bug in the engine: bind every id as a SERIAL-typed `Value`
* Change the schema's keys from SERIAL to INT64
* Build against a local checkout with `RE_LADYBUG_SOURCE_DIR`

## Decision Outcome

Chosen option: **patch Ladybug in our fork, pinned by tag**, because it fixes
the planner for every query, not just the ones that remember a workaround. It
is also the case ADR 0001's survivability argument anticipated: patching the
C++ source ourselves.

Specifics:

* The fork is `github.com/broadya/ladybug`. Each patched version is an
  upstream release tag plus our commits, on a branch `rainbohz/<upstream tag>`,
  tagged `<upstream tag>-rainbohz.<n>`. The first is `v0.20.4-rainbohz.1`.
* The patch changes `isConstantExpression` in
  `src/optimizer/filter_push_down_optimizer.cpp`. It also counts a cast as
  constant when that cast is between SERIAL and INT64, and no other cast.
  SERIAL is stored as INT64, so this cast cannot change a value or fail. A
  narrowing cast, such as an INT64 value against an INT32 key, still scans, as
  it does upstream. An optimizer test pins both behaviours.
* `rendering-engine/cmake/Dependencies.cmake` points `RE_LADYBUG_GIT_REPOSITORY`
  at the fork and `RE_LADYBUG_GIT_TAG` at the tag.

### Consequences

* Good, because a SERIAL key compared with an INT64 literal or parameter uses
  the hash index, whatever the query text.
* Good, because the pin is still an exact, immutable tag, as ADR 0001 requires.
* Bad, because every upstream bump now means rebasing our branch and cutting a
  new tag, not just moving the pin.
* Bad, because the build now depends on a personal fork being available. It is
  one variable away from upstream if the patch is merged there.
* Neutral, because the linked library still reports version 0.20.4. The fork
  does not change Ladybug's own version number, so the reported version does not
  show the patch. The pinned tag in `Dependencies.cmake` does.
* Neutral, because the storage format is unchanged, so the Homebrew shell at the
  same upstream version can still open databases the engine writes.

Impact on components:

Superseded: ADR 0011 now governs all of these.

* `rendering-engine` — its build fetched Ladybug from the fork, through
  `RE_LADYBUG_GIT_REPOSITORY` and `RE_LADYBUG_GIT_TAG` in
  `rendering-engine/cmake/Dependencies.cmake`, and bound ids as plain
  `int64_t` with no workaround at the call sites.
* `engine-db` — kept its SERIAL keys, and its lookups by id became index
  scans.

### Confirmation

`SerialPrimaryKeyEqualityWithInt64ValueUsesIndexScan` in the fork's
`test/optimizer/optimizer_test.cpp` checks the plan for an INT64 literal and
for a bound INT64 parameter against a SERIAL key, and that a narrowing cast
still scans. When bumping, run it on the rebased branch before tagging.

## Pros and Cons of the Options

### Patch Ladybug in our fork

* Good, because it fixes the cause once, for all callers.
* Bad, because it is a patch to carry until upstream takes it.

### Bind ids as SERIAL-typed Values

* Good, because it needs no change to Ladybug.
* Bad, because it depends on every call site remembering, and nothing fails when
  one forgets; the query is only slower. It also does not cover literals in
  query text.
* Bad, because the obvious way to build such a Value,
  `Value::createDefaultValue(SERIAL)`, silently returns an INT64 Value.

### Change SERIAL keys to INT64

* Good, because INT64 parameters against INT64 keys already use the index.
* Bad, because it gives up SERIAL's key generation, which the schema relies on,
  and moves id allocation into our own code.

### Local checkout via `RE_LADYBUG_SOURCE_DIR`

* Good, because it needs nothing published.
* Bad, because it is not reproducible on a clean machine, which ADR 0001
  rejects for the default build.

## More Information

* ADR 0001, which this extends. The pinning mechanism is unchanged; only the
  source repository and the tag differ.
* AGENTS.md, "Survivability", for why patching Ladybug's C++ source ourselves
  was always an accepted route.
* The fork's commit, "Use the primary-key index for a SERIAL key compared with
  an INT64 value", explains the fix.
* Revisit when upstream merges an equivalent fix. The pin can then return to an
  upstream tag.
