---
status: accepted
date: 2026-09-18
---

# Load Cypher into `engine-db` in process, statement by statement

## Context and Problem Statement

`rendering-engine` hosts `engine-db` in process and starts from an empty
database. Before it can do anything it has to apply the schema, and in test
mode it then has to apply an initial database state from a Cypher file named on
the command line.

[ADR 0002](0002-apply-ddl-one-statement-per-process.md) already decided how DDL
reaches Ladybug: one statement per `lbug` process, driven from Python. That
decision was made for the build tooling — the validator and the database
loader — on a machine that may have no C++ toolchain configured. It explicitly
listed "a small C++ loader linked against Ladybug" among the options it did not
take, and recorded the condition under which it would become the right one:
*"this becomes the natural choice if the loader ever has to do bulk data
loading, where per-statement process cost would dominate."*

That condition has arrived, and with a second one ADR 0002 did not anticipate:
the engine's database is **in memory**. There is no path on disk for an `lbug`
process to attach to. A subprocess loader is not merely slow here; it cannot
work at all.

So: how does the engine put Cypher into its own database?

## Decision Drivers

* The engine's `engine-db` is in-process and in-memory. Any loader that is a
  separate process is excluded by construction.
* An initial state file is data, not DDL. A score at target scale is 1M+
  partials; ~60 ms of process startup per statement is not a cost that can be
  paid per node.
* Errors must still be attributable to a source line. This is the property
  ADR 0002 was protecting, and it does not become less valuable in C++.
* A statement that does not run must never be reported as success — the failure
  mode that made the original `init_db.sh` untenable.
* The engine should not need a second opinion about where a statement ends. If
  it and the Python tooling disagree about statement boundaries, they report
  different line numbers for the same error in the same file.

## Considered Options

* Hand the whole file to `lbug::main::Connection::query` in one call
* Split in C++ and submit one statement per `query` call
* Shell out to the `lbug` CLI, as the Python tooling does
* Have the Python tooling generate a single combined statement

## Decision Outcome

Chosen option: **split in C++ and submit one statement per `query` call**,
against the in-process connection, collecting every failure rather than
stopping at the first.

The three `lbug` behaviours ADR 0002 documents — errors on stdout, exit 0 on
failure, statements silently skipped when a whole file is piped in — are
properties of the **CLI**, not of Ladybug. The embedded API reports failure
through `QueryResult::isSuccess()` and `getErrorMessage()`, which is an
unambiguous per-statement signal. So the reason for one statement per
submission is no longer correctness: it is **attribution**. One input, one
result, one line range.

The splitter is a port of `split_statements()` in `scripts/ladybug_ddl.py`,
which stays the reference implementation.

Failures do not stop the run. Every statement is attempted and all failures are
reported together, because the common case when a state file is wrong is that
several statements are wrong for the same reason, and finding them one build at
a time is the slower path. The engine then exits non-zero.

### Consequences

* Good, because the engine can load its own in-memory database at all, which
  no subprocess-based loader can do.
* Good, because per-statement cost falls from a process (~60 ms) to a function
  call, which is what makes an initial state file of any size viable.
* Good, because diagnostics keep the line attribution ADR 0002 established, and
  the engine's output is deliberately in the same shape as `load_schema.py`'s.
* Good, because the loader sees exactly the Ladybug the engine is linked
  against (ADR 0001), rather than whatever the Homebrew `lbug` happens to be.
  This is a stronger check than `make validate-schemas` can make, and
  `ScriptLoader.RealEngineSchemaLoadsInProcess` makes it.
* Bad, because the statement splitter now exists twice, in two languages. They
  have to be kept in step, and a divergence would show up as the two tools
  disagreeing about which lines an error belongs to. Both files say so, and the
  C++ tests cover the same cases.
* Neutral, because this does not supersede ADR 0002. The Python tooling keeps
  the CLI path, and must: `make validate-schemas` runs as the first step of
  `make test`, in seconds, with no compiler involved. That property is worth
  more than consolidating the two loaders.

Impact on components:

* `rendering-engine` — applies the schema and any `--state` file itself,
  through `ScriptLoader`, attempting every statement and exiting non-zero if
  any failed. Its splitter in `cypher_script.cpp` is kept in step with
  `scripts/ladybug_ddl.py`.
* `engine-db` — in memory, so it is populated only through this in-process
  loader. No external tool can attach to it.
* `hzl-compiler` — the Cypher it emits is loaded through this path, one
  statement per call, so a load error is reported against a line of the
  emitted file. At target scale, bulk loading may move to `COPY FROM`, which
  this decision does not cover.
* Build system — unchanged: the Python tooling keeps the CLI path of ADR 0002.

### Confirmation

`make test-rendering-engine`. The loader tests cover attribution across single
and multi-line statements, continuing past a failure, and idempotent
re-application; `ScriptLoader.RealEngineSchemaLoadsInProcess` applies the
actual `engine-db/schema/` and fails if any statement does not execute.

As of this ADR that test fails, and correctly: `CREATE REL TABLE
AGGREGATE_OF_ONE_MANY` at lines 395-401 of `engine-schema.cypher` names
`PartialSet`, which is declared at line 555. `make validate-schemas` reports
the same defect at the same lines, which is the parity between the two loaders
being demonstrated rather than a problem with either.

Note (2026-09-29): `AGGREGATE_OF_ONE_MANY` has since been replaced by four
relationships, `HAS_SOUNDENTITY`, `HAS_SOUNDSOURCE`, `HAS_PARTIALSET` and
`HAS_PARTIAL`, each declared after the node tables it connects, and the test
now passes. The paragraph above records the schema as it stood when this ADR
was accepted; the decision is unchanged.

## Pros and Cons of the Options

### Hand the whole file to `Connection::query`

* Good, because it is one call, and the API does return a chain of results.
* Bad, because a failure is then attributable to nothing more precise than the
  file. For a hand-edited 967-line schema that is close to useless.
* Bad, because it requires trusting that the chain contains one result per
  statement in order, which is exactly the assumption the CLI violated.

### Split in C++, one statement per query call (chosen)

* Good, because attribution is exact and the cost is a function call.
* Bad, because the splitter is duplicated across two languages.

### Shell out to the `lbug` CLI

* Good, because it would reuse the tooling that already exists.
* Bad, because it cannot reach an in-memory database at all.
* Bad, because it would make the engine depend on an external binary at
  runtime, and on a Ladybug build that is not the one it is linked against.

### Generate one combined statement from Python

* Good, because it would keep a single splitter.
* Bad, because it puts a Python dependency in the engine's runtime path, for a
  component whose whole point is being a self-contained C++ binary.

## More Information

* `rendering-engine/src/cypher_script.cpp` — the splitter, and the note that
  `scripts/ladybug_ddl.py` is the reference implementation.
* `rendering-engine/src/script_loader.cpp` — the loader.
* [ADR 0002](0002-apply-ddl-one-statement-per-process.md) for the build
  tooling's path, which is unchanged.
* [ADR 0001](0001-build-ladybug-from-pinned-source.md) for why the engine's
  Ladybug and the Homebrew `lbug` can differ in the first place.
* `rendering-engine/docs/design.md` for how the engine starts from an empty
  database and applies the schema and state.
