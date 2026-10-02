---
status: accepted
date: 2026-10-02
decision-makers: Alan Broady
consulted: Claude (design discussion)
---

# Serve REST with cpp-httplib, one call at a time, holding one score

## Context and Problem Statement

Until now `rendering-engine` ran only as a batch: load a score, render the
frames of a request file, exit. Every run rebuilt `engine-db` from nothing.
The target is a long-running engine that keeps `engine-db` in memory between
operations. The author decided (2026-10-02) that, before the message bus
exists, it should take its operations as REST calls: load a score from a
URI, render one frame and return the file's URI, and shut down. The server
sits behind an API gateway in any real deployment, so it needs no
authentication or hardening of its own.

That leaves four questions. Which HTTP library? How are concurrent calls
handled? What does a load do when a score is already loaded? And how does an
embedded HTTP library coexist with the one Ladybug already carries?

## Decision Drivers

* **The dependency rules of AGENTS.md.** Built from pinned source via
  FetchContent; included as a SYSTEM header; never exposed in a public
  header of this component.
* **Weight.** Three endpoints with JSON bodies. Nothing about the service
  needs asynchronous I/O, routing beyond exact paths, or middleware.
* **One `engine-db`, one score.** Every call reads or replaces the same
  database. Concurrency would buy contention and locking and, for two loads
  at once, a question with no good answer.
* **Mainstream and maintained.** The library should be one a C++ reader is
  likely to know, with regular releases.

## Considered Options

For the library:

1. **cpp-httplib** (`yhirose/cpp-httplib`): one header, MIT, blocking
   sockets with a pluggable task queue, frequent releases, widely used.
2. **Boost.Beast**: industrial, asynchronous, and a great deal of code for a
   small REST surface; it brings Boost and Asio.
3. **A web framework** (Drogon, Crow): routing, JSON and middleware built in,
   with dependencies and opinions to match.

For concurrency:

1. **One call at a time, on the accepting thread**, with no worker threads.
2. **A worker pool** with an engine-wide lock.
3. **Asynchronous jobs**: return 202 and a job id; the client polls.

## Decision Outcome

**cpp-httplib, with every call handled on the thread that accepted it.** The
library's task queue is replaced by one that runs each connection inline, so
the accept loop serves one connection to completion before it accepts the
next. Keep-alive is limited to one request per connection, so a client that
holds a connection open cannot hold the server.

A pool behind a lock serialises the work anyway, while adding threads that
have nothing to do but wait. Asynchronous jobs add a job table, a status
endpoint and a lifecycle for results, for a service whose callers can simply
wait. Both remain available later, if the bus brings real parallelism.

**A load replaces the score.** A `PUT /v1/score` refused for its request (a bad URI,
a path outside the input root, a missing file) changes nothing. One that
proceeds discards the held score first, then loads into a fresh `engine-db`;
if that load fails, the server holds no score and renders are refused until a
load succeeds. The alternative, building the new score beside the old and
swapping only on success, keeps a working score through a bad load, at the
cost of two databases in memory during every load. The author preferred that
a failed replacement leave nothing, so that a caller who asked for a new score
can never render the old one by mistake. Comparing the new score with the old
and recomputing only the difference is the dirty-flag work still to come
(`rendering-engine/docs/design.md` §4), not part of this decision.

**nlohmann/json** carries the bodies: header-only, MIT, and the JSON library a
C++ reader most likely already knows. It puts its symbols in a versioned
inline namespace, so it cannot collide with another copy.

### Ladybug's copy of cpp-httplib

Ladybug compiles cpp-httplib 0.14.2 into its extension installer
(`src/extension/extension_installer.cpp`, with OpenSSL support), in
namespace `httplib`. Linking a second version into the same binary is an ODR
violation: the linker keeps one definition of each inline function and vtable,
and the program runs a mix of the two. The first build did exactly that, and
crashed with "pure virtual function called" on its first connection.

So `src/server.cpp` includes our copy with the namespace renamed, by
defining `httplib` as `rainbohz_httplib` around the `#include` and refers to
it through the alias `http`. Every symbol of our copy then has its own name,
and the two coexist. The alternatives were worse:

* **Using Ladybug's copy** ties the server to whatever version Ladybug
  vendors (0.14.2 is from 2023), and that copy is compiled with OpenSSL
  support, which changes class layouts; including it without the same macros
  would be the same ODR violation in another form.
* **Patching the fork** to rename Ladybug's copy adds a patch to carry
  through every Ladybug release, for a problem that can be solved on our
  side.
* **Another library** gives up the one that best fits the drivers, for a
  conflict a one-line rename resolves.

The rename depends on the token `httplib` appearing in the header only as
the library's own name, which is true of a library whose every identifier is
in that namespace. It must be re-checked when cpp-httplib is bumped: the
server tests (`test_server.cpp` and the CTest `server.*`) exercise the real
socket path, and a collision shows up there as a crash.

Upstream's CMake for cpp-httplib is not run at all. It turns on HTTPS when it
finds OpenSSL, which Ladybug guarantees it will, and compression when it finds
zlib, brotli or zstd. FetchContent only fetches the header, and the build
exposes it as an INTERFACE target with no `CPPHTTPLIB_*_SUPPORT` defined:
plain HTTP, as a service behind a gateway needs.

### Consequences

* Good, because the server adds two headers and no build systems, and HTTP is
  confined to one source file; everything the server decides is tested
  without a socket, through `ServerCore`.
* Good, because one call at a time needs no locking, and no caller can see a
  half-loaded score.
* Bad, because a long score load delays every call behind it, including a
  `POST /v1/shutdown`.
* Bad, because a failed score load leaves the server with nothing to render
  until the next good one.
* Neutral, because the namespace rename is unusual, and only stays correct
  while it is re-checked at each bump of cpp-httplib.

## More Information

`rendering-engine/api/openapi.yaml` specifies the calls, and
`rendering-engine/docs/server.md` describes them; `rendering-engine/docs/design.md`
§3 places them among the engine's ingresses. Their paths, bodies and error
format are the conventions of [ADR 0017](0017-rest-resources-under-v1-described-by-openapi-3-1.md).
