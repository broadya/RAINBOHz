---
status: accepted
date: 2026-09-27
---

# Carry upstream's SERIAL primary-key fix in our fork until a release includes it

## Context and Problem Statement

ADR 0010 moved the build to our Ladybug fork to carry a fix of our own: Ladybug
v0.20.4 does not use the primary-key hash index when a SERIAL key is compared
with an INT64 value, so every id bound from C++ cost a full table scan.

We then reported the problem upstream (LadybugDB/ladybug issue #1025). The
maintainers fixed it within a day, in PR #1028, merged to `main` on 2026-09-25.
Their fix differs from ours. It treats any cast of a constant as constant,
where ours accepted only the SERIAL/INT64 cast, and it adds a guard in the
secondary-index path that the broader rule needs.

The fix is merged but not released. The latest release is still v0.20.4. What
should the fork carry until a release includes it?

## Decision Drivers

* ADR 0001: track the latest stable release, and know exactly which source is
  compiled in.
* The fork should differ from upstream as little as possible, so that leaving it
  is trivial.
* Lookups must not scan whole tables.

## Considered Options

* Keep our own narrower fix until the release
* Backport upstream's fix onto v0.20.4
* Build from upstream `main` at the merge commit

## Decision Outcome

Chosen option: **backport upstream's fix onto v0.20.4**, because the fork then
carries upstream's code rather than a variant of it. When the release arrives
nothing changes in behaviour, and the fork can simply be dropped.

Specifics:

* The fork branch `rainbohz/v0.20.4` is tagged `v0.20.4-rainbohz.2`. On top of
  v0.20.4 it carries:
  * upstream commits `5a22ad4a1` and `974151ebf` (PR #1028), cherry-picked
    unchanged;
  * the optimizer hunk only of upstream `e82d769f0`. It keeps scan + filter
    for columnar-backed tables (icebug-disk, Arrow), which have no usable
    primary-key index. Upstream's fix relies on it, and without it an icebug-disk
    end-to-end test returns wrong results. The rest of that commit is an export
    feature we do not take;
  * our regression test, adapted to the broader rule. Upstream's PR has no
    test of its own.
* `src/optimizer/filter_push_down_optimizer.cpp` on the tag is identical to
  upstream `main` at `974151ebf`.
* `rendering-engine/cmake/Dependencies.cmake` pins `v0.20.4-rainbohz.2`.

This supersedes ADR 0010. The mechanism (fork, branch, tag, pin) is unchanged;
what the fork carries, and when it ends, are what changed.

### Consequences

* Good, because SERIAL lookups use the hash index with the code upstream will
  ship. A SERIAL-keyed lookup that read 100,000 rows now reads one.
* Good, because leaving the fork is a pin change, with no behaviour change to
  validate beyond the release itself.
* Neutral, because the broader rule also applies to other key types: an INT64
  value against an INT32 key, such as `Frame.frame_number`, now uses the index
  too. Results are unchanged, including the overflow error for a value outside
  the INT32 range.
* Bad, because until the release the build still depends on the personal fork.

Impact on components:

* `rendering-engine` — `rendering-engine/cmake/Dependencies.cmake` pins
  `v0.20.4-rainbohz.2` from the fork, until a Ladybug release includes
  PR #1028. Ids are bound as plain `int64_t`, with no workaround at the call
  sites.
* `engine-db` — keeps its SERIAL keys. Lookups by id, and by narrower integer
  keys such as `Frame.frame_number`, use the primary-key index.

### Confirmation

On the fork build: `optimizer_test` passes 20 of 20, including
`SerialPrimaryKeyEqualityWithInt64ValueUsesIndexScan`, and the end-to-end suite
passes 1962 of 1963. The one failure, `AnonymousParquetDeleteReload`, also
fails on unmodified v0.20.4. `PROFILE MATCH (n:T) WHERE n.id = 7` on a
100,000-row SERIAL table plans as `PRIMARY_KEY_SCAN_NODE_TABLE` and reads one
row.

Leave the fork when a Ladybug release includes PR #1028. Pin that release from
`LadybugDB/ladybug`, and check with `PROFILE` that a SERIAL lookup still reads
one row.

## Pros and Cons of the Options

### Keep our own narrower fix

* Good, because it is already built and tested.
* Bad, because it behaves differently from upstream, for example on INT32 keys.
  Switching to the release would then change behaviour, which is the point we
  would least want a surprise.

### Backport upstream's fix onto v0.20.4

* Good, because the fork carries upstream's code on a released base.
* Bad, because a backport needs care. The fix alone was not enough on v0.20.4:
  it also needed a guard that `main` had gained in between.

### Build from upstream `main`

* Good, because it needs no backport.
* Bad, because `main` is not a release. It has over a hundred unreleased
  commits, which ADR 0001's "latest stable release" rules out.

## More Information

* ADR 0010, superseded by this ADR.
* LadybugDB/ladybug issue #1025 and PR #1028.
* The tag message of `v0.20.4-rainbohz.2` in `github.com/broadya/ladybug`
  lists the backported commits.
* ADR 0001, whose pinning policy this follows.
