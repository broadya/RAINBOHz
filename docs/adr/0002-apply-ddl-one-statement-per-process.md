---
status: accepted
date: 2026-08-14
---

# Apply LadybugDB DDL one statement per process, and detect failure from output

## Context and Problem Statement

Schema DDL in `canonical-db/schema/` and `rendering-db/schema/` is the source of
truth for the object model. Two things need to drive it into a real database:
a validator (does this schema load at all?) and a loader (apply it to a database).

The obvious implementation is to pipe the `.cypher` file into the `lbug` shell.
That is what `canonical-db/init_db.sh` originally did, and it silently produced
half-loaded databases while reporting success. How should DDL be submitted to
Ladybug, and how should failure be detected?

## Decision Drivers

* A partially-applied schema must never be reported as success. The schema is
  the object model; a missing table becomes a wrong query result much later.
* Errors must be attributable to a source line. The schema files are hand-edited
  deliberately, so a diagnostic that does not say *where* is of little use.
* No build step should be required. Validation runs in `make test` ahead of any
  compilation, on a machine that may have no configured C++ toolchain.
* There are no Ladybug Python bindings installed, and no packaged ones for this
  fork at the time of writing.

## Considered Options

* Pipe the whole `.cypher` file into `lbug` (the status quo)
* Normalise to one statement per line, single `lbug` process, pair up outputs
* Submit one statement per `lbug` process against a persistent database
* Write a small C++ loader linked against Ladybug, as `rendering-engine` does
* Use Python bindings

## Decision Outcome

Chosen option: **submit one statement per `lbug` process**, against a persistent
database so that inter-statement dependencies still resolve, detecting failure by
scanning output for error markers rather than by exit code.

This is driven by three behaviours of the `lbug` CLI, each established by
experiment against the Homebrew build of 0.19.1:

1. Failures are reported on **stdout**, not stderr.
2. It exits **0** even when statements fail.
3. Fed a whole schema file on stdin, it **silently skips** some statements —
   they neither execute nor report an error. The same statement succeeds when
   submitted on its own.

(3) is what makes the status quo untenable. It is not a reporting weakness that
better error handling could compensate for: work is simply not done, with no
diagnostic. Piping `rendering-schema.cypher` in whole created 27 of its tables;
submitting the same statements individually created all of them.

Given (3), any option that hands `lbug` more than one statement at a time has to
prove that nothing was dropped — which requires per-statement accounting anyway.
At that point one process per statement is the simpler construction.

### Consequences

* Good, because a statement either executes or is reported. There is no third
  outcome, which is exactly what the status quo got wrong.
* Good, because errors carry a source line range, since the splitter tracks
  positions. A missing semicolon shows up as the merged line span it actually
  produces.
* Good, because it needs nothing but Python and the `lbug` binary. Schema
  validation runs first in `make test`, before any compiler is involved.
* Bad, because it costs one process per statement — roughly 60 ms, so a few
  seconds for a whole schema. Acceptable for a ~45-statement DDL file run at
  build time; it would not be acceptable for bulk data loading, which is a
  different problem needing a different mechanism.
* Bad, because the splitter is our own code, and Cypher lexical rules
  (comments, string literals, quoted identifiers) are reimplemented well enough
  for DDL rather than completely.
* Neutral, because these are defects in a specific CLI version. If a later
  Ladybug fixes (1)-(3), or bindings become available, this decision should be
  revisited — the behaviours are documented in `scripts/ladybug_ddl.py` so the
  premise can be re-tested directly.

Impact on components:

* `engine-db` — its schema, `engine-db/schema/engine-schema.cypher`, is
  validated and applied by this path (`make validate-engine-schema`,
  `make init-engine-db`, `make reset-engine-db`). Hand edits to it must stay
  within the DDL the splitter understands: statements ended by `;`, and
  comments, string literals and quoted identifiers as Cypher writes them.
* `rendering-engine` — does not submit DDL through `lbug`; it loads Cypher in
  process (ADR 0005). Its C++ splitter is a port of `split_statements()` in
  `scripts/ladybug_ddl.py`, so a change to the splitting rules here must be
  made in both places.
* `hzl-compiler` — the Cypher it emits is split by that port, so it inherits
  the same lexical rules.
* Build system — `make test` runs `validate-schemas` first, and needs only
  Python and the `lbug` binary to do so.

### Confirmation

`make validate-schemas`, which runs as the first step of `make test`, and fails
non-zero if any statement in any schema does not execute.

The loader is covered by the same mechanism: `make init-*-db` and
`make reset-*-db` exit non-zero and report the offending statement when DDL
fails, which the original shell script could not do.

## Pros and Cons of the Options

### Pipe the whole file into `lbug`

* Good, because it is a one-line script.
* Bad, because statements are silently skipped, so databases half-load.
* Bad, because the exit code says nothing, and errors land on stdout where a
  conventional `2>&1`-style capture misses them.
* Bad, because there is no way to attribute an error to a source line.

### One statement per line, single process, pair up outputs

* Good, because it is a single process, so it is fast.
* Bad, because output pairing is a heuristic: successes print a result table,
  failures print a line of text, and matching them back to inputs is guesswork.
* Bad, because it does not remove the skipping behaviour, only makes it harder
  to notice.
* Bad, because collapsing statements onto single lines destroys the line
  numbers that make diagnostics useful.

### One statement per process (chosen)

* Good, because attribution is exact — one input, one result.
* Good, because it requires no build.
* Bad, because of process-per-statement cost.

### Small C++ loader linked against Ladybug

* Good, because it drives the C++ API directly and sidesteps the shell entirely.
* Good, because it would share the pinned Ladybug version with the engine
  (ADR 0001), so the loader and the renderer agree on storage format.
* Bad, because it makes schema validation depend on a full C++ build, including
  compiling Ladybug from source the first time. Validation would stop being the
  cheap first step of `make test`.
* Neutral, because this becomes the natural choice if the loader ever has to do
  bulk data loading, where per-statement process cost would dominate.

### Python bindings

* Good, because it would be the most direct route from the Python build steps.
* Bad, because none are installed, and packaging for this fork is not something
  the project controls.

## More Information

* `scripts/ladybug_ddl.py` — the shared splitter, executor and error detection,
  and the single place where the `lbug` behaviours above are documented.
* `scripts/load_schema.py` — the loader; `scripts/validate_ladybug_schema.py`
  — the validator. Both import the shared module.
* AGENTS.md, "Schema Validation", which repeats the warning at the point of use.
* ADR 0001 for how the compiled components obtain Ladybug, which is a separate
  question from how DDL is submitted to the shell.
